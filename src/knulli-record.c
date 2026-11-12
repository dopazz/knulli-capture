/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * knulli-record - record the display(s) and the audio to an MKV or MP4
 * file.
 *
 * C replacement for the knulli-record shell script (ffmpeg kmsgrab/fbdev +
 * PulseAudio). Built to run next to an emulator on 4-core ARM handhelds
 * (Allwinner H700 / A133P, Rockchip RK3326 / RK3566) without getting in its
 * way:
 *
 *   - Screens are read the way knulli-screenshot reads them (kms.c): DRM
 *     planes, composited when an overlay such as the RG-DS keyboard is up,
 *     or /dev/fb0. Several screens are stacked into one picture.
 *   - On Rockchip SoCs the RGA 2D engine reads each screen's buffer and
 *     writes it, converted to YUV (and turned or halved), straight into the
 *     picture: ~2 ms of its time and almost no CPU (rga.c).
 *   - On Allwinner SoCs with the vendor display driver and ION (H700 /
 *     H616), the display engine itself writes the finished screen into
 *     memory as YUV ("write-back"), so the framebuffer is never read
 *     (sunxi.c).
 *   - Otherwise (no RGA, overlays to composite, fbdev): scanout memory is
 *     uncached on these devices, so each row is pulled out once with wide
 *     loads and converted to YUV while it is still in the cache (NEON for
 *     the common upright 32-bit case).
 *   - A screen whose framebuffer has not flipped since the last frame is
 *     not read or encoded at all; the file has a variable frame rate.
 *   - If encoding falls behind, frames are dropped instead of queued, so
 *     the recording stays in real time and memory stays flat.
 *   - x264 (statically linked) runs ultrafast/zerolatency on two threads by
 *     default; audio is the PulseAudio monitor of the default output,
 *     encoded to Opus, or to AAC for MP4 (vo-aacenc), both statically
 *     linked. libpulse-simple is loaded at run time, so the recorder still
 *     works, video only, without PulseAudio.
 *   - The process runs at nice 10 and idle I/O priority by default.
 *
 * Files are named knulli-record_<wwww>x<h>_<timestamp>.mkv (or .mp4), the
 * width zero-padded to 4 digits. Stop with Ctrl+C / SIGINT or SIGTERM (or -t);
 * the file is finished cleanly. Must run as root, like knulli-screenshot.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/fb.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include <x264.h>
#include <opus/opus.h>
#include <vo-aacenc/voAAC.h>
#include <vo-aacenc/cmnMemory.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#include "kms.h"
#include "mkv.h"
#include "mp4.h"
#include "rga.h"
#include "sunxi.h"
#include "yuv.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define DEFAULT_DIR    "/userdata/recordings"
#define DEFAULT_PREFIX "knulli-record"
#define MAX_MAPS       8      /* framebuffers kept mapped */
#define KEEPALIVE_MS   1000   /* re-read an unchanged screen this often */
#define MUX_DELAY_MS   300    /* how long a packet may wait for the other track */
#define AUDIO_RATE     48000
#define AUDIO_CH       2
#define OPUS_FRAME     960    /* samples per channel per packet: 20 ms */
#define AAC_FRAME      1024   /* AAC-LC frame: 21.3 ms */
#define AAC_DELAY      1600   /* vo-aacenc's delay: a frame + 576 lookahead */

static volatile sig_atomic_t stop;

static int64_t now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* ------------------------------------------------------------------------ */
/* Packet queue: interleaves video and audio in timestamp order             */
/* ------------------------------------------------------------------------ */

struct packet {
    struct packet *next;
    int track, key;
    int64_t ts;
    size_t len;
    uint8_t data[];
};

struct queue {
    struct packet *head[3], **tail[3]; /* by track: 1 video, 2 audio */
    pthread_mutex_t lock;
};

static struct queue q;

static void queue_init(void)
{
    int i;

    pthread_mutex_init(&q.lock, NULL);
    for (i = 0; i < 3; i++) {
        q.head[i] = NULL;
        q.tail[i] = &q.head[i];
    }
}

static int queue_push(int track, int64_t ts, int key, const uint8_t *data,
                      size_t len)
{
    struct packet *p = malloc(sizeof(*p) + len);

    if (!p)
        return -1;
    p->next = NULL;
    p->track = track;
    p->key = key;
    p->ts = ts;
    p->len = len;
    memcpy(p->data, data, len);
    pthread_mutex_lock(&q.lock);
    *q.tail[track] = p;
    q.tail[track] = &p->next;
    pthread_mutex_unlock(&q.lock);
    return 0;
}

/* The output file: MKV or MP4. */
struct output {
    struct mkv *mkv;
    struct mp4 *mp4;
};

static int output_write(struct output *o, const struct packet *p)
{
    if (o->mp4)
        return mp4_write(o->mp4, p->track, p->ts, p->key, p->data, p->len);
    return mkv_write(o->mkv, p->track, p->ts, p->key, p->data, p->len);
}

static int output_close(struct output *o, int64_t duration_ms)
{
    int ret = 0;

    if (o->mp4)
        ret = mp4_close(o->mp4, duration_ms);
    else if (o->mkv)
        ret = mkv_close(o->mkv, duration_ms);
    o->mp4 = NULL;
    o->mkv = NULL;
    return ret;
}

/*
 * Write queued packets oldest first. A packet is written once the other
 * track has a later one queued, or once it is older than `until` (the other
 * track has nothing to interleave), or always with `until` = INT64_MAX.
 */
static int queue_drain(struct output *out, int64_t until)
{
    int ret = 0;

    for (;;) {
        struct packet *v, *a, *p;

        pthread_mutex_lock(&q.lock);
        v = q.head[MKV_VIDEO];
        a = q.head[MKV_AUDIO];
        if (v && a)
            p = a->ts < v->ts ? a : v;
        else if (v && v->ts <= until)
            p = v;
        else if (a && a->ts <= until)
            p = a;
        else
            p = NULL;
        if (p) {
            q.head[p->track] = p->next;
            if (!p->next)
                q.tail[p->track] = &q.head[p->track];
        }
        pthread_mutex_unlock(&q.lock);
        if (!p)
            return ret;
        if (output_write(out, p) != 0)
            ret = -1;
        free(p);
    }
}

/* ------------------------------------------------------------------------ */
/* Audio: PulseAudio monitor -> Opus, AAC (for MP4) or PCM                  */
/* ------------------------------------------------------------------------ */

struct audio {
    pa_simple *pa;
    OpusEncoder *opus;     /* NULL and no AAC: store PCM */
    VO_AUDIO_CODECAPI aac; /* vo-aacenc, when aac_h is set */
    VO_HANDLE aac_h;
    VO_MEM_OPERATOR aac_mem;
    uint8_t asc[2];        /* AAC AudioSpecificConfig */
    int frame;             /* samples per channel per packet */
    int bitrate;
    uint8_t head[19];      /* OpusHead */
    int preskip;
    int64_t t0;            /* recording start, CLOCK_MONOTONIC ns */
    pthread_t thread;
    int running;
    int64_t packets, resyncs;
};

static struct {
    void *lib;
    pa_simple *(*new_)(const char *, const char *, pa_stream_direction_t,
                       const char *, const char *, const pa_sample_spec *,
                       const pa_channel_map *, const pa_buffer_attr *, int *);
    int (*read)(pa_simple *, void *, size_t, int *);
    pa_usec_t (*latency)(pa_simple *, int *);
    void (*free_)(pa_simple *);
} pa;

static int pulse_load(void)
{
    if (pa.lib)
        return 0;
    pa.lib = dlopen("libpulse-simple.so.0", RTLD_NOW);
    if (!pa.lib) {
        logv("audio: %s\n", dlerror());
        return -1;
    }
    *(void **)&pa.new_ = dlsym(pa.lib, "pa_simple_new");
    *(void **)&pa.read = dlsym(pa.lib, "pa_simple_read");
    *(void **)&pa.latency = dlsym(pa.lib, "pa_simple_get_latency");
    *(void **)&pa.free_ = dlsym(pa.lib, "pa_simple_free");
    if (!pa.new_ || !pa.read || !pa.latency || !pa.free_) {
        dlclose(pa.lib);
        pa.lib = NULL;
        return -1;
    }
    return 0;
}

/* AAC-LC through vo-aacenc: raw frames (no ADTS), for MP4. */
static int aac_open(struct audio *a, int kbps)
{
    VO_CODEC_INIT_USERDATA ud;
    AACENC_PARAM prm;

    voGetAACEncAPI(&a->aac);
    a->aac_mem.Alloc = cmnMemAlloc;
    a->aac_mem.Copy = cmnMemCopy;
    a->aac_mem.Free = cmnMemFree;
    a->aac_mem.Set = cmnMemSet;
    a->aac_mem.Check = cmnMemCheck;
    ud.memflag = VO_IMF_USERMEMOPERATOR;
    ud.memData = &a->aac_mem;
    if (a->aac.Init(&a->aac_h, VO_AUDIO_CodingAAC, &ud) != VO_ERR_NONE) {
        a->aac_h = NULL;
        return -1;
    }
    memset(&prm, 0, sizeof(prm));
    prm.sampleRate = AUDIO_RATE;
    prm.bitRate = kbps * 1000;
    prm.nChannels = AUDIO_CH;
    prm.adtsUsed = 0;
    if (a->aac.SetParam(a->aac_h, VO_PID_AAC_ENCPARAM, &prm) != VO_ERR_NONE) {
        a->aac.Uninit(a->aac_h);
        a->aac_h = NULL;
        return -1;
    }
    /* AAC LC (2), 48 kHz (index 3), stereo */
    a->asc[0] = (uint8_t)(2 << 3 | 3 >> 1);
    a->asc[1] = (uint8_t)((3 & 1) << 7 | AUDIO_CH << 3);
    a->bitrate = kbps * 1000;
    return 0;
}

static int audio_open(struct audio *a, const char *source, int kbps, int aac)
{
    pa_sample_spec ss = { PA_SAMPLE_S16LE, AUDIO_RATE, AUDIO_CH };
    pa_buffer_attr ba;
    int e = 0;

    memset(a, 0, sizeof(*a));
    a->frame = aac ? AAC_FRAME : OPUS_FRAME;
    if (pulse_load() != 0) {
        err("audio: libpulse-simple not available, recording video only\n");
        return -1;
    }
    /* Deliver a packet's worth at a time; let the server buffer as much as
     * it likes. */
    ba.maxlength = (uint32_t)-1;
    ba.tlength = (uint32_t)-1;
    ba.prebuf = (uint32_t)-1;
    ba.minreq = (uint32_t)-1;
    ba.fragsize = (uint32_t)(a->frame * AUDIO_CH * 2);
    a->pa = pa.new_(NULL, "knulli-record", PA_STREAM_RECORD, source,
                    "recording", &ss, NULL, &ba, &e);
    if (!a->pa) {
        err("audio: cannot record from %s (PulseAudio error %d), recording "
            "video only\n", source, e);
        return -1;
    }

    if (aac) {
        if (aac_open(a, kbps) != 0) {
            err("audio: cannot start the AAC encoder at %d kbit/s\n", kbps);
            pa.free_(a->pa);
            a->pa = NULL;
            return -1;
        }
    } else if (kbps > 0) {
        a->opus = opus_encoder_create(AUDIO_RATE, AUDIO_CH,
                                      OPUS_APPLICATION_AUDIO, &e);
        if (!a->opus) {
            err("audio: opus: %s\n", opus_strerror(e));
            pa.free_(a->pa);
            a->pa = NULL;
            return -1;
        }
        a->bitrate = kbps * 1000;
        opus_encoder_ctl(a->opus, OPUS_SET_BITRATE(a->bitrate));
        opus_encoder_ctl(a->opus, OPUS_SET_COMPLEXITY(5)); /* half the CPU of 10 */
        opus_encoder_ctl(a->opus, OPUS_GET_LOOKAHEAD(&a->preskip));
        memcpy(a->head, "OpusHead", 8);
        a->head[8] = 1;                       /* version */
        a->head[9] = AUDIO_CH;
        a->head[10] = (uint8_t)a->preskip;    /* little endian */
        a->head[11] = (uint8_t)(a->preskip >> 8);
        a->head[12] = (uint8_t)AUDIO_RATE;
        a->head[13] = (uint8_t)(AUDIO_RATE >> 8);
        a->head[14] = (uint8_t)(AUDIO_RATE >> 16);
        a->head[15] = (uint8_t)(AUDIO_RATE >> 24);
        /* 16..17 output gain 0, 18 channel mapping family 0 */
    }
    logv("audio: %s, %s\n", source,
         a->aac_h ? "AAC" : a->opus ? "Opus" : "16-bit PCM");
    return 0;
}

static void *audio_thread(void *arg)
{
    struct audio *a = arg;
    int16_t pcm[AAC_FRAME * AUDIO_CH];
    size_t pcm_len = (size_t)a->frame * AUDIO_CH * 2;
    int64_t frame_ms = (int64_t)a->frame * 1000 / AUDIO_RATE;
    uint8_t pkt[8192];
    int64_t samples = 0, base = 0;
    int first = 1, e;

    while (!stop) {
        int64_t at, ts;
        int len;

        if (pa.read(a->pa, pcm, pcm_len, &e) < 0) {
            err("audio: read failed (PulseAudio error %d), audio stops here\n", e);
            break;
        }
        /* When the last sample of this chunk was captured, in ms. */
        at = (now_ns() - a->t0) / 1000000 -
             (int64_t)(pa.latency(a->pa, &e) / 1000);
        ts = base + samples * 1000 / AUDIO_RATE;
        /* Anchor on the first chunk; re-anchor if the stream had a gap. */
        if (first || at - frame_ms - ts > 200 || ts - (at - frame_ms) > 200) {
            if (!first)
                a->resyncs++;
            base = at - frame_ms;
            samples = 0;
            ts = base;
            first = 0;
        }
        samples += a->frame;
        if (ts < 0)
            continue; /* captured before the recording started */

        if (a->aac_h) {
            VO_CODECBUFFER in = { 0 }, out = { 0 };
            VO_AUDIO_OUTPUTINFO info = { 0 };

            in.Buffer = (VO_PBYTE)pcm;
            in.Length = (VO_U32)pcm_len;
            a->aac.SetInputData(a->aac_h, &in);
            out.Buffer = pkt;
            out.Length = sizeof(pkt);
            if (a->aac.GetOutputData(a->aac_h, &out, &info) != VO_ERR_NONE ||
                !out.Length) {
                err("audio: AAC encoding failed\n");
                continue;
            }
            queue_push(MKV_AUDIO, ts, 1, pkt, out.Length);
        } else if (a->opus) {
            len = opus_encode(a->opus, pcm, a->frame, pkt, sizeof(pkt));
            if (len < 0) {
                err("audio: opus: %s\n", opus_strerror(len));
                continue;
            }
            queue_push(MKV_AUDIO, ts, 1, pkt, (size_t)len);
        } else {
            queue_push(MKV_AUDIO, ts, 1, (const uint8_t *)pcm, pcm_len);
        }
        a->packets++;
    }
    return NULL;
}

static int audio_start(struct audio *a, int64_t t0)
{
    sigset_t all, old;
    int ret;

    a->t0 = t0;
    /* Signals go to the main thread. */
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    ret = pthread_create(&a->thread, NULL, audio_thread, a);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (ret != 0)
        return -1;
    a->running = 1;
    return 0;
}

static void audio_close(struct audio *a)
{
    if (a->running)
        pthread_join(a->thread, NULL);
    if (a->pa)
        pa.free_(a->pa);
    if (a->opus)
        opus_encoder_destroy(a->opus);
    if (a->aac_h)
        a->aac.Uninit(a->aac_h);
    a->aac_h = NULL;
    a->running = 0;
    a->pa = NULL;
    a->opus = NULL;
}

/* ------------------------------------------------------------------------ */
/* Video capture                                                            */
/* ------------------------------------------------------------------------ */

/* One screen's place in the recorded picture. */
struct slot {
    struct screen scr;
    struct rect r;        /* in the picture, turned upright */
    int uw, uh;           /* its size the way the screen is oriented */
    uint32_t sig[4 * MAX_PLANES + 4]; /* what was on screen last time */
    int nsig;
};

/* A framebuffer kept mapped between frames. */
struct cached_map {
    struct fbmap m;
    ino_t ino;            /* identifies the buffer behind a reused fb id */
    int64_t used;
    uint8_t *copy;        /* its pixels in cached memory (overlays) */
    size_t copy_len;
    int have_copy;
    uint32_t rga;         /* registered with the RGA, or 0 */
};

struct capture {
    int fd;               /* DRM, or -1 for fbdev */
    struct plane planes[MAX_PLANES];
    uint32_t plane_fbw[MAX_PLANES], plane_fbh[MAX_PLANES];
    int np;
    struct slot slots[MAX_SCREENS];
    int ns;
    int turns, half;
    struct cached_map maps[MAX_MAPS];
    /* fbdev */
    int fbfd;
    uint8_t *fbmem;
    size_t fblen;
    /* scratch */
    uint8_t *rows;        /* two rows of pixels */
    size_t rows_len;
    uint8_t *stage;       /* a whole layer, for the general path */
    size_t stage_len;
    uint8_t *comp;        /* a composited screen, BGRX */
    size_t comp_len;
    uint8_t *turn;        /* an upright YUV picture, before turning it */
    size_t turn_len;
    /* RGA */
    int use_rga;
    uint32_t rga_pic;     /* the picture, registered with the RGA */
    int pic_w, pic_h;
    int64_t rga_frames;   /* screens the RGA drew */
    /* Allwinner display-engine capture */
    int use_sx;
    struct sunxi_cap sx;
    int64_t sx_frames;
    int64_t sx_missed;    /* ticks the engine skipped, read by CPU instead */
    uint8_t *rga_comp;    /* a screen being composited by the RGA, BGRX */
    uint32_t rga_comp_h;  /* ...registered with the RGA */
    int rga_comp_w, rga_comp_hh;
    uint32_t bad_fb;      /* last fb we complained about */
};

static int grow(uint8_t **p, size_t *len, size_t need)
{
    uint8_t *n;

    if (need <= *len)
        return 0;
    n = realloc(*p, need);
    if (!n) {
        err("out of memory\n");
        return -1;
    }
    *p = n;
    *len = need;
    return 0;
}

/* Identify the buffer behind fb `id` (fb ids are reused after a free). */
static int fb_identity(int fd, uint32_t id, struct drm_fb *fb, ino_t *ino)
{
    struct drm_prime_handle prime;
    struct stat st;

    *ino = 0;
    if (drm_query_fb(fd, id, fb) != 0)
        return -1;
    memset(&prime, 0, sizeof(prime));
    prime.handle = fb->handle;
    prime.flags = DRM_CLOEXEC;
    if (fb->handle && xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0) {
        if (fstat(prime.fd, &st) == 0)
            *ino = st.st_ino;
        close(prime.fd);
    }
    gem_close(fd, fb->handle);
    fb->handle = 0;
    return 0;
}

static struct cached_map *get_map(struct capture *c, uint32_t id, int64_t now)
{
    struct cached_map *cm, *victim = NULL;
    struct drm_fb fb;
    ino_t ino;
    int i;

    if (fb_identity(c->fd, id, &fb, &ino) != 0)
        return NULL;
    for (i = 0; i < MAX_MAPS; i++) {
        cm = &c->maps[i];
        if (cm->m.map && cm->m.fb_id == id && cm->ino == ino &&
            cm->m.fb.width == fb.width && cm->m.fb.height == fb.height &&
            cm->m.fb.fourcc == fb.fourcc && cm->m.fb.pitch == fb.pitch) {
            cm->used = now;
            return cm;
        }
        if (!victim || !cm->m.map || (victim->m.map && cm->used < victim->used))
            victim = cm;
    }
    if (victim->m.map)
        drm_unmap_fb(&victim->m);
    rga_release(victim->rga);
    victim->rga = 0;
    victim->have_copy = 0;
    quiet = c->bad_fb == id;
    if (drm_map_fb(c->fd, id, &victim->m) != 0) {
        quiet = 0;
        c->bad_fb = id;
        return NULL;
    }
    quiet = 0;
    victim->ino = ino;
    victim->used = now;
    return victim;
}

/* Let go of buffers nobody has shown for a while (they pin memory). */
static void expire_maps(struct capture *c, int64_t now)
{
    int i;

    for (i = 0; i < MAX_MAPS; i++)
        if (c->maps[i].m.map && now - c->maps[i].used > 2000000000LL) {
            drm_unmap_fb(&c->maps[i].m);
            rga_release(c->maps[i].rga);
            c->maps[i].rga = 0;
            c->maps[i].have_copy = 0;
        }
}

/*
 * Where a rectangle of screen `sl` (in its mode_w x mode_h coordinates)
 * lands in the picture, turned upright and aligned to whole 2x2 blocks.
 */
static struct rect place(const struct slot *sl, int turns, int64_t x, int64_t y,
                         int64_t w, int64_t h, uint32_t mode_w, uint32_t mode_h)
{
    int64_t ux0 = x * sl->uw / mode_w, ux1 = (x + w) * sl->uw / mode_w;
    int64_t uy0 = y * sl->uh / mode_h, uy1 = (y + h) * sl->uh / mode_h;
    int64_t rx, ry, rw, rh, x0, y0, x1, y1;
    struct rect r;

    switch (turns) {
    case 1:  rx = sl->uh - uy1; ry = ux0; rw = uy1 - uy0; rh = ux1 - ux0; break;
    case 2:  rx = sl->uw - ux1; ry = sl->uh - uy1; rw = ux1 - ux0; rh = uy1 - uy0; break;
    case 3:  rx = uy0; ry = sl->uw - ux1; rw = uy1 - uy0; rh = ux1 - ux0; break;
    default: rx = ux0; ry = uy0; rw = ux1 - ux0; rh = uy1 - uy0; break;
    }
    x0 = sl->r.x + (rx & ~1LL);
    y0 = sl->r.y + (ry & ~1LL);
    x1 = sl->r.x + ((rx + rw + 1) & ~1LL);
    y1 = sl->r.y + ((ry + rh + 1) & ~1LL);
    if (x0 < sl->r.x)
        x0 = sl->r.x;
    if (y0 < sl->r.y)
        y0 = sl->r.y;
    if (x1 > sl->r.x + sl->r.w)
        x1 = sl->r.x + sl->r.w;
    if (y1 > sl->r.y + sl->r.h)
        y1 = sl->r.y + sl->r.h;
    r.x = (int)x0;
    r.y = (int)y0;
    r.w = x1 > x0 ? (int)(x1 - x0) : 0;
    r.h = y1 > y0 ? (int)(y1 - y0) : 0;
    return r;
}

/* How a pixel format can be read: natively if it is 8 bits per channel. */
static int native_layout(const struct pixfmt *f, struct rgbsrc *s)
{
    if (f->r_len != 8 || f->g_len != 8 || f->b_len != 8 ||
        (f->bpp != 32 && f->bpp != 24) ||
        (f->r_off | f->g_off | f->b_off) & 7)
        return 0;
    s->bpp = f->bpp / 8;
    s->ri = f->r_off / 8;
    s->gi = f->g_off / 8;
    s->bi = f->b_off / 8;
    return 1;
}

/*
 * Draw `sw` x `sh` pixels at `src` (rows `pitch` apart, live scanout memory)
 * into picture rectangle `dst`. Upright, unscaled, 8-bit channels: two rows
 * at a time straight into YUV. Anything else: copy (and widen to RGB24 if
 * needed) into cached memory first, then the general converter.
 */
static int draw(struct capture *c, struct yuv *f, const struct rect *dst,
                const uint8_t *src, size_t pitch, uint32_t sw, uint32_t sh,
                const struct pixfmt *fmt)
{
    struct rgbsrc s;
    int native = native_layout(fmt, &s);
    size_t rowbytes = (size_t)sw * (fmt->bpp / 8);
    uint32_t y;

    if (!dst->w || !dst->h || !sw || !sh)
        return 0;

    if (native && !c->turns && (uint32_t)dst->w == sw && (uint32_t)dst->h == sh) {
        if (grow(&c->rows, &c->rows_len, 2 * rowbytes + 64) != 0)
            return -1;
        for (y = 0; y + 1 < sh; y += 2) {
            fetch_bytes(c->rows, src + (size_t)y * pitch, rowbytes);
            fetch_bytes(c->rows + rowbytes, src + (size_t)(y + 1) * pitch,
                        rowbytes);
            yuv_put_rows(f, dst->x, dst->y + (int)y, c->rows,
                         c->rows + rowbytes, dst->w, &s);
        }
        return 0;
    }

    if (native && c->turns && !(sw & 1) && !(sh & 1) &&
        (uint32_t)dst->w == ((c->turns & 1) ? sh : sw) &&
        (uint32_t)dst->h == ((c->turns & 1) ? sw : sh)) {
        /* A panel mounted sideways: convert upright two rows at a time,
         * then turn the finished Y, U and V planes. */
        struct yuv tmp;
        size_t ysize = (size_t)sw * sh;

        if (grow(&c->rows, &c->rows_len, 2 * rowbytes + 64) != 0 ||
            grow(&c->turn, &c->turn_len, ysize + ysize / 2) != 0)
            return -1;
        tmp.w = (int)sw;
        tmp.h = (int)sh;
        tmp.p[0] = c->turn;
        tmp.p[1] = c->turn + ysize;
        tmp.p[2] = c->turn + ysize + ysize / 4;
        tmp.stride[0] = (int)sw;
        tmp.stride[1] = tmp.stride[2] = (int)sw / 2;
        for (y = 0; y < sh; y += 2) {
            fetch_bytes(c->rows, src + (size_t)y * pitch, rowbytes);
            fetch_bytes(c->rows + rowbytes, src + (size_t)(y + 1) * pitch,
                        rowbytes);
            yuv_put_rows(&tmp, 0, (int)y, c->rows, c->rows + rowbytes,
                         (int)sw, &s);
        }
        yuv_rotate(f, dst, &tmp, c->turns);
        return 0;
    }

    if (native) {
        if (grow(&c->stage, &c->stage_len, rowbytes * sh) != 0)
            return -1;
        for (y = 0; y < sh; y++)
            fetch_bytes(c->stage + y * rowbytes, src + (size_t)y * pitch,
                        rowbytes);
        s.pitch = rowbytes;
    } else {
        /* Other layouts (RGB565, 10-bit, ...): widen to RGB24 row by row. */
        int bytes = fmt->bpp / 8;
        uint32_t x;

        if (grow(&c->stage, &c->stage_len, (size_t)sw * 3 * sh) != 0 ||
            grow(&c->rows, &c->rows_len, rowbytes + 64) != 0)
            return -1;
        for (y = 0; y < sh; y++) {
            const uint8_t *p = c->rows;
            uint8_t *d = c->stage + (size_t)y * sw * 3;

            fetch_bytes(c->rows, src + (size_t)y * pitch, rowbytes);
            for (x = 0; x < sw; x++, p += bytes) {
                uint32_t v = 0;
                int i;

                for (i = 0; i < bytes; i++)
                    v |= (uint32_t)p[i] << (8 * i);
                *d++ = extract(v, fmt->r_off, fmt->r_len);
                *d++ = extract(v, fmt->g_off, fmt->g_len);
                *d++ = extract(v, fmt->b_off, fmt->b_len);
            }
        }
        s.bpp = 3;
        s.ri = 0;
        s.gi = 1;
        s.bi = 2;
        s.pitch = (size_t)sw * 3;
    }
    s.data = c->stage;
    s.w = (int)sw;
    s.h = (int)sh;
    yuv_put_image(f, dst, &s, c->turns);
    return 0;
}

/*
 * Draw source rectangle sx,sy,sw,sh of framebuffer `fb_id` at dx,dy,dw,dh on
 * the screen of `sl` (sw/sh 0: to the buffer's edge; dw/dh 0: unscaled).
 */
static int draw_fb(struct capture *c, struct yuv *f, const struct slot *sl,
                   uint32_t fb_id, uint32_t sx, uint32_t sy, uint32_t sw,
                   uint32_t sh, int64_t dx, int64_t dy, int64_t dw, int64_t dh,
                   int64_t now)
{
    const struct screen *scr = &sl->scr;
    struct cached_map *cm = get_map(c, fb_id, now);
    struct fbmap *m;
    struct rect r;
    int ret;

    if (!cm)
        return -1;
    m = &cm->m;
    /* Keep the source inside the buffer. */
    if (sx >= m->fb.width || sy >= m->fb.height)
        return -1;
    if (!sw || sw > m->fb.width - sx)
        sw = m->fb.width - sx;
    if (!sh || sh > m->fb.height - sy)
        sh = m->fb.height - sy;
    if (!dw || !dh) {
        dw = sw;
        dh = sh;
    }
    /* Clip to the screen, trimming the source to match. */
    if (dx < 0) {
        uint32_t cut = (uint32_t)(-dx * sw / dw);

        sx += cut;
        sw -= cut;
        dw += dx;
        dx = 0;
    }
    if (dy < 0) {
        uint32_t cut = (uint32_t)(-dy * sh / dh);

        sy += cut;
        sh -= cut;
        dh += dy;
        dy = 0;
    }
    if (dx + dw > scr->mode_w) {
        int64_t over = dx + dw - scr->mode_w;

        sw -= (uint32_t)(over * sw / dw);
        dw -= over;
    }
    if (dy + dh > scr->mode_h) {
        int64_t over = dy + dh - scr->mode_h;

        sh -= (uint32_t)(over * sh / dh);
        dh -= over;
    }
    if (dw <= 0 || dh <= 0 || !sw || !sh)
        return 0;

    r = place(sl, c->turns, dx, dy, dw, dh, scr->mode_w, scr->mode_h);

    if (c->use_rga) {
        /* Let the RGA read, convert, turn and scale it into the picture. */
        int fmt = rga_format(m->fb.fourcc);

        if (fmt >= 0 && m->dmafd >= 0 && !m->fb.offset && !(m->fb.pitch & 3) &&
            r.w > 0 && r.h > 0) {
            struct rect sr = { (int)sx, (int)sy, (int)sw, (int)sh };
            const char *why = "";

            if (!cm->rga)
                cm->rga = rga_import_fd(m->dmafd, (int)(m->fb.pitch / 4),
                                        (int)m->fb.height, fmt);
            if (cm->rga &&
                rga_blit(cm->rga, (int)m->fb.width, (int)m->fb.height,
                         (int)(m->fb.pitch / 4), fmt, &sr, c->rga_pic,
                         c->pic_w, c->pic_h, &r, c->turns, &why) == 0) {
                c->rga_frames++;
                return 0;
            }
            err("RGA failed (%s), converting on the CPU from now on\n",
                cm->rga ? why : "cannot import buffer");
            c->use_rga = 0;
        }
    }

    dmabuf_sync(m->dmafd, 0);
    ret = draw(c, f, &r, m->map + m->fb.offset + (size_t)sy * m->fb.pitch +
               (size_t)sx * (m->fb.fmt->bpp / 8), m->fb.pitch, sw, sh,
               m->fb.fmt);
    dmabuf_sync(m->dmafd, 1);
    return ret;
}

/* Read pixel `p` of format `f` as R, G, B, A. */
static inline void read_px(const uint8_t *p, const struct pixfmt *f,
                           const struct rgbsrc *nat, int ai, uint8_t *rgba)
{
    if (nat) {
        rgba[0] = p[nat->ri];
        rgba[1] = p[nat->gi];
        rgba[2] = p[nat->bi];
        rgba[3] = ai >= 0 ? p[ai] : 255;
    } else {
        uint32_t v = 0;
        int i;

        for (i = 0; i < f->bpp / 8; i++)
            v |= (uint32_t)p[i] << (8 * i);
        rgba[0] = extract(v, f->r_off, f->r_len);
        rgba[1] = extract(v, f->g_off, f->g_len);
        rgba[2] = extract(v, f->b_off, f->b_len);
        rgba[3] = f->a_len ? extract(v, f->a_off, f->a_len) : 255;
    }
}

static inline uint8_t div255(uint32_t x)
{
    return (uint8_t)((x + 128 + ((x + 128) >> 8)) >> 8);
}

#if defined(__aarch64__)
/* (x + 127.5) / 255 for 16-bit lanes holding up to 255 * 255. */
static inline uint8x8_t div255_8(uint16x8_t x)
{
    return vraddhn_u16(x, vrshrq_n_u16(x, 8));
}

/*
 * One row of an unscaled 8-bit-per-channel plane with alpha onto BGRX, 8
 * pixels at a time. `ri`..`ai` are the source byte positions; returns how
 * many pixels were done (the rest is left to the C loop).
 */
static uint32_t blend_row_neon(uint8_t *d, const uint8_t *s, uint32_t n,
                               int ri, int gi, int bi, int ai, uint32_t pa,
                               int premulti)
{
    uint8x8_t vpa = vdup_n_u8((uint8_t)pa), k255 = vdup_n_u8(255);
    uint32_t x;

    for (x = 0; x + 8 <= n; x += 8) {
        uint8x8x4_t sv = vld4_u8(s + 4 * x), dv = vld4_u8(d + 4 * x);
        uint8x8_t a = div255_8(vmull_u8(sv.val[ai], vpa));
        uint8x8_t ia = vsub_u8(k255, a);
        uint8x8_t m = premulti ? vpa : a; /* what the colour is scaled by */
        uint8x8_t r = sv.val[ri], g = sv.val[gi], b = sv.val[bi];

        dv.val[2] = div255_8(vmlal_u8(vmull_u8(r, m), dv.val[2], ia));
        dv.val[1] = div255_8(vmlal_u8(vmull_u8(g, m), dv.val[1], ia));
        dv.val[0] = div255_8(vmlal_u8(vmull_u8(b, m), dv.val[0], ia));
        vst4_u8(d + 4 * x, dv);
    }
    return x;
}
#endif

/*
 * Blend layer `l` (pixels at `px`, rows `pitch` apart, format `f`) onto the
 * BGRX screen `comp` (cw x ch), scaled nearest-neighbour to its place, the
 * way the display controller mixes it.
 */
static void blend_plane(uint8_t *comp, uint32_t cw, uint32_t ch,
                        const struct plane *l, const uint8_t *px, size_t pitch,
                        uint32_t fbw, uint32_t fbh, const struct pixfmt *f)
{
    uint32_t sx0 = l->src_x >> 16, sy0 = l->src_y >> 16;
    uint32_t sw = l->src_w >> 16, sh = l->src_h >> 16;
    uint32_t pa = l->alpha >> 8, bytes = f->bpp / 8;
    struct rgbsrc ns;
    const struct rgbsrc *nat = native_layout(f, &ns) ? &ns : NULL;
    int ai = (f->a_len == 8 && !(f->a_off & 7) && l->blend != BLEND_NONE) ?
             f->a_off / 8 : -1;
    int64_t dx, dy;

    if (sx0 >= fbw || sy0 >= fbh || !l->crtc_w || !l->crtc_h)
        return;
    if (!sw || sw > fbw - sx0)
        sw = fbw - sx0;
    if (!sh || sh > fbh - sy0)
        sh = fbh - sy0;

    for (dy = l->crtc_y < 0 ? -l->crtc_y : 0;
         dy < l->crtc_h && l->crtc_y + dy < ch; dy++) {
        uint32_t sy = sy0 + (uint32_t)(dy * sh / l->crtc_h);
        const uint8_t *srow = px + (size_t)sy * pitch;
        uint8_t *drow = comp + (size_t)(l->crtc_y + dy) * cw * 4;

        dx = l->crtc_x < 0 ? -l->crtc_x : 0;
#if defined(__aarch64__)
        /* Unscaled 32-bit plane with alpha (the RG-DS keyboard): NEON. */
        if (nat && bytes == 4 && ai >= 0 && sw == l->crtc_w) {
            int64_t end = l->crtc_w;

            if (l->crtc_x + end > cw)
                end = cw - l->crtc_x;
            if (end > dx)
                dx += blend_row_neon(drow + (size_t)(l->crtc_x + dx) * 4,
                                     srow + (size_t)(sx0 + dx) * 4,
                                     (uint32_t)(end - dx), nat->ri, nat->gi,
                                     nat->bi, ai, pa,
                                     l->blend == BLEND_PREMULTI);
        }
#endif
        for (; dx < l->crtc_w && l->crtc_x + dx < cw; dx++) {
            uint32_t sx = sx0 + (uint32_t)(dx * sw / l->crtc_w);
            uint8_t *d = drow + (size_t)(l->crtc_x + dx) * 4;
            uint8_t s[4];
            uint32_t a;

            read_px(srow + (size_t)sx * bytes, f, nat, ai, s);
            if (l->blend == BLEND_NONE || !f->a_len)
                s[3] = 255;
            a = s[3] * pa;               /* 0..255*255 */
            if (a >= 255 * 255) {
                d[2] = s[0];
                d[1] = s[1];
                d[0] = s[2];
                continue;
            }
            a = div255(a);
            if (!a && l->blend != BLEND_PREMULTI)
                continue;
            if (l->blend == BLEND_PREMULTI) { /* colour already scaled */
                d[2] = div255(s[0] * pa + d[2] * (255 - a));
                d[1] = div255(s[1] * pa + d[1] * (255 - a));
                d[0] = div255(s[2] * pa + d[0] * (255 - a));
            } else {
                d[2] = div255(s[0] * a + d[2] * (255 - a));
                d[1] = div255(s[1] * a + d[1] * (255 - a));
                d[0] = div255(s[2] * a + d[0] * (255 - a));
            }
        }
    }
}

/*
 * Composite a screen's planes with the RGA: the bottom plane into a
 * screen-sized buffer, each plane above blended over it the way the display
 * controller blends it, then the result converted (turned, scaled) into the
 * picture. Returns 0; -1 if this layout needs the CPU path (a plane off the
 * screen or the buffer, a format the RGA cannot read); -2 if the RGA failed.
 */
static int rga_compose(struct capture *c, const struct slot *sl,
                       struct plane **l, int n, int64_t now)
{
    int cw = (int)sl->scr.mode_w, ch = (int)sl->scr.mode_h, i;
    struct rect full = { 0, 0, cw, ch };
    const char *why = "";

    if (cw != c->rga_comp_w || ch != c->rga_comp_hh) {
        rga_release(c->rga_comp_h);
        free(c->rga_comp);
        c->rga_comp_h = 0;
        c->rga_comp_w = c->rga_comp_hh = 0;
        c->rga_comp = aligned_alloc(4096, ((size_t)cw * ch * 4 + 4095) &
                                          ~(size_t)4095);
        if (!c->rga_comp)
            return -1;
        c->rga_comp_h = rga_import_mem(c->rga_comp, cw, ch, rga_bgrx());
        if (!c->rga_comp_h)
            return -2;
        c->rga_comp_w = cw;
        c->rga_comp_hh = ch;
    }

    for (i = 0; i < n; i++) {
        const struct plane *pl = l[i];
        struct cached_map *cm = get_map(c, pl->fb_id, now);
        const struct drm_fb *fb;
        struct rect sr, dr;
        int fmt, alpha = (int)(pl->alpha >> 8), mode;

        if (!cm)
            return -1;
        fb = &cm->m.fb;
        sr.x = (int)(pl->src_x >> 16);
        sr.y = (int)(pl->src_y >> 16);
        sr.w = (int)(pl->src_w >> 16);
        sr.h = (int)(pl->src_h >> 16);
        dr.x = pl->crtc_x;
        dr.y = pl->crtc_y;
        dr.w = (int)pl->crtc_w;
        dr.h = (int)pl->crtc_h;
        if (cm->m.dmafd < 0 || fb->offset || (fb->pitch & 3) ||
            sr.w <= 0 || sr.h <= 0 || sr.x + sr.w > (int)fb->width ||
            sr.y + sr.h > (int)fb->height || dr.w <= 0 || dr.h <= 0 ||
            dr.x < 0 || dr.y < 0 || dr.x + dr.w > cw || dr.y + dr.h > ch)
            return -1;

        /* What the plane's pixels and blend mode call for. */
        if (pl->blend == BLEND_NONE) {
            fmt = rga_format(fb->fourcc);       /* alpha channel ignored */
            mode = RGA_OVER_COVERAGE;           /* only plane opacity left */
        } else {
            fmt = rga_format_alpha(fb->fourcc);
            mode = pl->blend == BLEND_PREMULTI ? RGA_OVER_PREMULT
                                               : RGA_OVER_COVERAGE;
        }
        if (fmt < 0)
            return -1;
        if (fmt == rga_format(fb->fourcc) && alpha == 255)
            mode = RGA_COPY;                    /* opaque: no blending */
        if (i == 0 && (mode != RGA_COPY || dr.x || dr.y || dr.w != cw ||
                       dr.h != ch))
            memset(c->rga_comp, 0, (size_t)cw * ch * 4); /* black below */

        if (!cm->rga)
            cm->rga = rga_import_fd(cm->m.dmafd, (int)(fb->pitch / 4),
                                    (int)fb->height, fmt);
        if (!cm->rga ||
            rga_draw(cm->rga, (int)fb->width, (int)fb->height,
                     (int)(fb->pitch / 4), fmt, &sr, alpha, c->rga_comp_h, cw,
                     ch, rga_bgrx(), &dr, 0, mode, &why) != 0)
            goto fail;
    }
    if (rga_draw(c->rga_comp_h, cw, ch, cw, rga_bgrx(), &full, 255,
                 c->rga_pic, c->pic_w, c->pic_h, rga_i420(), &sl->r, c->turns,
                 RGA_COPY, &why) != 0)
        goto fail;
    c->rga_frames++;
    return 0;

fail:
    err("RGA failed (%s), converting on the CPU from now on\n",
        why && *why ? why : "cannot import buffer");
    c->use_rga = 0;
    return -2;
}

/*
 * A screen showing several planes (the RG-DS keyboard over the game, a
 * cursor, a video layer): composite them in cached memory, then convert.
 * The bottom plane, which usually flips every frame, is read straight from
 * its buffer; the planes above keep a cached copy until their buffer
 * changes (or `force`), since reading scanout memory is what costs.
 */
static int compose(struct capture *c, struct yuv *f, const struct slot *sl,
                   struct plane **l, int n, int64_t now, int force)
{
    uint32_t cw = sl->scr.mode_w, ch = sl->scr.mode_h, y;
    struct rgbsrc s;
    int i, drawn = 0;

    if (c->use_rga && rga_compose(c, sl, l, n, now) == 0)
        return 0;
    if (grow(&c->comp, &c->comp_len, (size_t)cw * ch * 4) != 0)
        return -1;

    for (i = 0; i < n; i++) {
        struct cached_map *cm = get_map(c, l[i]->fb_id, now);
        const struct drm_fb *fb;
        const uint8_t *base;
        size_t rowbytes;

        if (!cm)
            continue;
        fb = &cm->m.fb;
        base = cm->m.map + fb->offset;
        rowbytes = (size_t)fb->width * (fb->fmt->bpp / 8);

        if (!drawn && l[i]->crtc_x == 0 && l[i]->crtc_y == 0 &&
            l[i]->crtc_w == cw && l[i]->crtc_h == ch &&
            l[i]->src_x == 0 && l[i]->src_y == 0 &&
            (l[i]->src_w >> 16) == cw && (l[i]->src_h >> 16) == ch &&
            fb->width >= cw && fb->height >= ch &&
            fb->fmt->bpp == 32 && native_layout(fb->fmt, &s) &&
            s.ri == 2 && s.gi == 1 && s.bi == 0) {
            /* Full-screen XRGB bottom plane: straight in, no blending. */
            dmabuf_sync(cm->m.dmafd, 0);
            for (y = 0; y < ch; y++)
                fetch_bytes(c->comp + (size_t)y * cw * 4,
                            base + (size_t)y * fb->pitch, (size_t)cw * 4);
            dmabuf_sync(cm->m.dmafd, 1);
            drawn++;
            continue;
        }
        if (!drawn)
            memset(c->comp, 0, (size_t)cw * ch * 4); /* black below */

        if (!cm->have_copy || force) {
            if (grow(&cm->copy, &cm->copy_len, rowbytes * fb->height) != 0)
                continue;
            dmabuf_sync(cm->m.dmafd, 0);
            for (y = 0; y < fb->height; y++)
                fetch_bytes(cm->copy + y * rowbytes, base + (size_t)y * fb->pitch,
                            rowbytes);
            dmabuf_sync(cm->m.dmafd, 1);
            cm->have_copy = 1;
        }
        blend_plane(c->comp, cw, ch, l[i], cm->copy, rowbytes, fb->width,
                    fb->height, fb->fmt);
        drawn++;
    }
    if (!drawn)
        return -1;
    s.data = c->comp;
    s.pitch = (size_t)cw * 4;
    s.w = (int)cw;
    s.h = (int)ch;
    s.bpp = 4;
    s.ri = 2;
    s.gi = 1;
    s.bi = 0;
    yuv_put_image(f, &sl->r, &s, c->turns);
    return 0;
}

/* The planes on screen `crtc`, bottom to top. */
static int screen_layers(struct capture *c, uint32_t crtc, struct plane **l)
{
    int i, n = 0;

    for (i = 0; i < c->np; i++)
        if (c->planes[i].crtc_id == crtc && c->planes[i].fb_id &&
            c->planes[i].have_geom)
            l[n++] = &c->planes[i];
    qsort(l, n, sizeof(l[0]), cmp_plane_z);
    return n;
}

/*
 * Refresh what each screen shows. Each tick costs one GETPLANE per plane and
 * one GETCRTC per screen; plane properties are only re-read when a plane is
 * switched on, moves to another screen, is an overlay (which may move), or
 * its screen changes mode. Returns 1 if anything on screen changed.
 */
static int drm_poll(struct capture *c)
{
    int i, j, changed = 0;

    for (i = 0; i < c->np; i++) {
        struct plane *p = &c->planes[i];
        struct drm_mode_get_plane gp;
        int reread;

        memset(&gp, 0, sizeof(gp));
        gp.plane_id = p->plane_id;
        if (xioctl(c->fd, DRM_IOCTL_MODE_GETPLANE, &gp) != 0)
            continue;
        reread = gp.crtc_id && gp.fb_id &&
                 (gp.crtc_id != p->crtc_id || !p->fb_id || p->type != 1);
        p->crtc_id = gp.crtc_id;
        p->fb_id = gp.fb_id;
        if (reread)
            plane_read_props(c->fd, p);
    }

    for (i = 0; i < c->ns; i++) {
        struct slot *sl = &c->slots[i];
        struct drm_mode_crtc cr;
        struct plane *l[MAX_PLANES];
        uint32_t sig[4 * MAX_PLANES + 4];
        int n, k = 0;

        memset(&cr, 0, sizeof(cr));
        cr.crtc_id = sl->scr.crtc_id;
        if (xioctl(c->fd, DRM_IOCTL_MODE_GETCRTC, &cr) == 0) {
            if (cr.mode_valid && (cr.mode.hdisplay != sl->scr.mode_w ||
                                  cr.mode.vdisplay != sl->scr.mode_h)) {
                logv("%s: mode now %ux%u\n", sl->scr.name, cr.mode.hdisplay,
                     cr.mode.vdisplay);
                for (j = 0; j < c->np; j++)
                    if (c->planes[j].crtc_id == cr.crtc_id &&
                        c->planes[j].fb_id)
                        plane_read_props(c->fd, &c->planes[j]);
                sl->scr.mode_w = cr.mode.hdisplay;
                sl->scr.mode_h = cr.mode.vdisplay;
            }
            sl->scr.fb_id = cr.mode_valid ? cr.fb_id : 0;
            sl->scr.x = cr.x;
            sl->scr.y = cr.y;
        }
        sig[k++] = sl->scr.fb_id;
        sig[k++] = sl->scr.mode_w;
        sig[k++] = sl->scr.mode_h;
        n = screen_layers(c, sl->scr.crtc_id, l);
        for (j = 0; j < n; j++) {
            sig[k++] = l[j]->fb_id;
            sig[k++] = (uint32_t)l[j]->crtc_x ^ ((uint32_t)l[j]->crtc_y << 16);
            sig[k++] = l[j]->crtc_w ^ (l[j]->crtc_h << 16);
            sig[k++] = l[j]->src_w ^ l[j]->src_h ^ l[j]->src_x ^ l[j]->src_y;
        }
        if (k != sl->nsig || memcmp(sig, sl->sig, k * sizeof(sig[0]))) {
            memcpy(sl->sig, sig, k * sizeof(sig[0]));
            sl->nsig = k;
            changed = 1;
        }
    }
    return changed;
}

/* Draw every screen into the picture (`force`: re-read cached overlays). */
static int drm_draw(struct capture *c, struct yuv *f, int64_t now, int force)
{
    int i, ok = 0;

    for (i = 0; i < c->ns; i++) {
        struct slot *sl = &c->slots[i];
        struct plane *l[MAX_PLANES];
        int n = screen_layers(c, sl->scr.crtc_id, l);

        if (n == 1 && l[0]->crtc_x == 0 && l[0]->crtc_y == 0 &&
            l[0]->crtc_w == sl->scr.mode_w && l[0]->crtc_h == sl->scr.mode_h &&
            (l[0]->blend == BLEND_NONE || l[0]->blend == BLEND_PREMULTI)) {
            /* The usual case: one plane fills the screen. */
            if (draw_fb(c, f, sl, l[0]->fb_id, l[0]->src_x >> 16,
                        l[0]->src_y >> 16, l[0]->src_w >> 16,
                        l[0]->src_h >> 16, 0, 0, sl->scr.mode_w,
                        sl->scr.mode_h, now) == 0) {
                ok++;
                continue;
            }
        } else if (n > 0) {
            /* Overlays (keyboard, cursor, video layer): composite them. */
            if (compose(c, f, sl, l, n, now, force) == 0) {
                ok++;
                continue;
            }
        } else if (sl->scr.fb_id) {
            /* No plane information (legacy KMS): the CRTC's framebuffer. */
            if (draw_fb(c, f, sl, sl->scr.fb_id, sl->scr.x, sl->scr.y,
                        sl->scr.mode_w, sl->scr.mode_h, 0, 0, sl->scr.mode_w,
                        sl->scr.mode_h, now) == 0) {
                ok++;
                continue;
            }
        }
        yuv_fill_black(f, &sl->r); /* screen off or unreadable */
    }
    return ok ? 0 : -1;
}

static int fbdev_draw(struct capture *c, struct yuv *f)
{
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    struct pixfmt fmt;
    struct slot *sl = &c->slots[0];
    size_t start, need;
    struct rect r;

    if (xioctl(c->fbfd, FBIOGET_VSCREENINFO, &vi) != 0 ||
        xioctl(c->fbfd, FBIOGET_FSCREENINFO, &fi) != 0)
        return -1;
    if (fi.smem_len != c->fblen) { /* the driver reallocated its memory */
        if (c->fbmem)
            munmap(c->fbmem, c->fblen);
        c->fblen = fi.smem_len;
        c->fbmem = mmap(NULL, c->fblen, PROT_READ, MAP_SHARED, c->fbfd, 0);
        if (c->fbmem == MAP_FAILED) {
            c->fbmem = NULL;
            return -1;
        }
    }
    fbdev_pixfmt(&vi, &fmt);
    start = (size_t)vi.yoffset * fi.line_length +
            (size_t)vi.xoffset * (vi.bits_per_pixel / 8);
    need = start + (size_t)fi.line_length * (vi.yres - 1) +
           (size_t)vi.xres * (vi.bits_per_pixel / 8);
    if (!vi.xres || !vi.yres || need > c->fblen || !c->fbmem ||
        (fmt.bpp != 16 && fmt.bpp != 24 && fmt.bpp != 32))
        return -1;
    if (vi.xres != sl->scr.mode_w || vi.yres != sl->scr.mode_h)
        yuv_fill_black(f, &sl->r); /* mode change: scaled to the old size */
    r = place(sl, c->turns, 0, 0, vi.xres, vi.yres, vi.xres, vi.yres);
    return draw(c, f, &r, c->fbmem + start, fi.line_length, vi.xres, vi.yres,
                &fmt);
}

/* Lay the screens out (stacked, turned upright, maybe halved): picture size. */
static void layout(struct capture *c, int *w, int *h)
{
    int i, y = 0, maxw = 0;

    for (i = 0; i < c->ns; i++) {
        struct slot *sl = &c->slots[i];
        int uw = (int)sl->scr.mode_w, uh = (int)sl->scr.mode_h;

        if (c->half) {
            uw /= 2;
            uh /= 2;
        }
        sl->uw = uw & ~1;
        sl->uh = uh & ~1;
        sl->r.x = 0;
        sl->r.y = y;
        sl->r.w = (c->turns & 1) ? sl->uh : sl->uw;
        sl->r.h = (c->turns & 1) ? sl->uw : sl->uh;
        y += sl->r.h;
        if (sl->r.w > maxw)
            maxw = sl->r.w;
    }
    *w = maxw;
    *h = y;
}

/*
 * Open the display: DRM screens (all active ones, those in `want`, or with
 * `active` the one EmulationStation is on), else /dev/fb0.
 */
static int capture_open(struct capture *c, const char *dev,
                        const struct screen_pick *want, int nwant, int reverse,
                        int active)
{
    struct screen scr[MAX_SCREENS];
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    struct screen_pick act;
    int i, n = 0;

    c->fbfd = -1;
    c->fd = drm_open(dev);
    if (c->fd >= 0) {
        c->np = drm_get_planes(c->fd, c->planes, MAX_PLANES);
        if (c->np < 0)
            c->np = 0;
        n = drm_get_screens(c->fd, scr, MAX_SCREENS, c->planes, c->np);
        if (n > 0 && active && !nwant) {
            char name[32] = "";

            memset(&act, 0, sizeof(act));
            act.crtc_id = drm_active_crtc(c->fd, name, sizeof(name));
            for (i = 0; i < n && scr[i].crtc_id != act.crtc_id; i++)
                ;
            if (act.crtc_id && i < n) {
                logv("active screen (/var/run/drmConn): %s, CRTC %u\n",
                     name, act.crtc_id);
                want = &act;
                nwant = 1;
            } else {
                err("no active screen in /var/run/drmConn, recording all "
                    "screens\n");
            }
        }
        if (n > 0)
            n = select_screens(scr, n, want, nwant, reverse);
        for (i = 0; i < n; i++) {
            memset(&c->slots[i], 0, sizeof(c->slots[i]));
            c->slots[i].scr = scr[i];
            logv("recording %s (CRTC %u) %ux%u\n", scr[i].name,
                 scr[i].crtc_id, scr[i].mode_w, scr[i].mode_h);
        }
        if (n > 0) {
            c->ns = n;
            return 0;
        }
        close(c->fd);
        c->fd = -1;
        if (dev || nwant > 0) {
            err("no usable DRM screen\n");
            return -1;
        }
    }

    c->fbfd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (c->fbfd < 0 || xioctl(c->fbfd, FBIOGET_VSCREENINFO, &vi) != 0 ||
        xioctl(c->fbfd, FBIOGET_FSCREENINFO, &fi) != 0 || !vi.xres ||
        !vi.yres) {
        err("no usable display found\n");
        return -1;
    }
    c->fblen = fi.smem_len;
    c->fbmem = mmap(NULL, c->fblen, PROT_READ, MAP_SHARED, c->fbfd, 0);
    if (c->fbmem == MAP_FAILED) {
        c->fbmem = NULL;
        err("/dev/fb0: mmap failed: %s\n", strerror(errno));
        return -1;
    }
    memset(&c->slots[0], 0, sizeof(c->slots[0]));
    c->slots[0].scr.mode_w = vi.xres;
    c->slots[0].scr.mode_h = vi.yres;
    strcpy(c->slots[0].scr.name, "fb0");
    c->ns = 1;
    logv("recording /dev/fb0 %ux%u bpp %u\n", vi.xres, vi.yres,
         vi.bits_per_pixel);
    return 0;
}

static void capture_close(struct capture *c)
{
    int i;

    for (i = 0; i < MAX_MAPS; i++) {
        if (c->maps[i].m.map)
            drm_unmap_fb(&c->maps[i].m);
        rga_release(c->maps[i].rga);
        free(c->maps[i].copy);
    }
    if (c->fbmem)
        munmap(c->fbmem, c->fblen);
    if (c->fbfd >= 0)
        close(c->fbfd);
    if (c->fd >= 0)
        close(c->fd);
    free(c->rows);
    free(c->stage);
    free(c->comp);
    free(c->turn);
    rga_release(c->rga_comp_h);
    free(c->rga_comp);
    if (c->use_sx)
        sunxi_close(&c->sx);
    c->use_sx = 0;
}

/*
 * One frame from the display engine: queue a capture for `ms` and place the
 * one queued two ticks ago, timed `*fms`. 0: drawn, 1: nothing ready yet,
 * -1: read the framebuffer instead, for this tick (the engine skipped it:
 * nothing was committed, e.g. a program draws into the shown page without
 * flipping) or from now on (capture failed).
 */
static int sunxi_draw(struct capture *c, struct yuv *f, int64_t ms,
                      int64_t *fms)
{
    struct slot *sl = &c->slots[0];
    struct yuv cy;
    int r = sunxi_step(&c->sx, ms, &cy, fms);

    if (r < 0) {
        err("display capture failed, reading the framebuffer from now on\n");
        sunxi_close(&c->sx);
        c->use_sx = 0;
        return -1;
    }
    if (r == 0)
        return 1;
    if (r == 2) {
        c->sx_missed++;
        return -1;
    }
    if (c->turns)
        yuv_rotate(f, &sl->r, &cy, c->turns);
    else
        yuv_copy(f, &sl->r, &cy);
    sunxi_release(&c->sx);
    c->sx_frames++;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* H.264                                                                    */
/* ------------------------------------------------------------------------ */

/* AVCDecoderConfigurationRecord from x264's SPS and PPS. */
static size_t make_avcc(x264_t *enc, uint8_t *out, size_t max)
{
    x264_nal_t *nals;
    const uint8_t *sps = NULL, *pps = NULL;
    int nn, i, sps_len = 0, pps_len = 0;
    size_t n = 0;

    if (x264_encoder_headers(enc, &nals, &nn) < 0)
        return 0;
    for (i = 0; i < nn; i++) {
        /* b_annexb = 0: each NAL starts with a 4-byte length. */
        if (nals[i].i_type == NAL_SPS) {
            sps = nals[i].p_payload + 4;
            sps_len = nals[i].i_payload - 4;
        } else if (nals[i].i_type == NAL_PPS) {
            pps = nals[i].p_payload + 4;
            pps_len = nals[i].i_payload - 4;
        }
    }
    if (!sps || !pps || sps_len < 4 || (size_t)(11 + sps_len + pps_len) > max)
        return 0;
    out[n++] = 1;          /* configurationVersion */
    out[n++] = sps[1];     /* profile */
    out[n++] = sps[2];     /* compatibility */
    out[n++] = sps[3];     /* level */
    out[n++] = 0xff;       /* 4-byte NAL lengths */
    out[n++] = 0xe1;       /* one SPS */
    out[n++] = (uint8_t)(sps_len >> 8);
    out[n++] = (uint8_t)sps_len;
    memcpy(out + n, sps, sps_len);
    n += sps_len;
    out[n++] = 1;          /* one PPS */
    out[n++] = (uint8_t)(pps_len >> 8);
    out[n++] = (uint8_t)pps_len;
    memcpy(out + n, pps, pps_len);
    n += pps_len;
    return n;
}

/* Encode `pic` (NULL: flush); queue what comes out. Returns bytes or -1. */
static int encode(x264_t *enc, x264_picture_t *pic, int64_t *bytes)
{
    x264_picture_t out;
    x264_nal_t *nals;
    int nn, size;

    size = x264_encoder_encode(enc, &nals, &nn, pic, &out);
    if (size < 0)
        return -1;
    if (size > 0) {
        /* The NALs of one frame lie back to back in memory. */
        if (queue_push(MKV_VIDEO, out.i_pts, out.b_keyframe,
                       nals[0].p_payload, (size_t)size) != 0)
            return -1;
        *bytes += size;
    }
    return size;
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
        "Usage: knulli-record [options] [file.mkv | file.mp4]\n"
        "Build: " BUILD_TIMESTAMP "\n"
        "\n"
        "Records all active displays (or those picked with -c) and the sound\n"
        "to an MKV file (H.264 + Opus), or with --mp4 an MP4 file (H.264 +\n"
        "AAC), until stopped with Ctrl+C, SIGINT or SIGTERM. Several\n"
        "displays are stacked vertically, highest CRTC id on top.\n"
        "\n"
        "Files are named\n"
        "  " DEFAULT_DIR "/" DEFAULT_PREFIX "_<wwww>x<h>_YYYYmmdd_HHhMMmSSs.mkv\n"
        "with the width zero-padded to 4 digits (.mp4 with --mp4). If\n"
        "file.mkv is given, the file is named file_<wwww>x<h>.mkv instead;\n"
        "a name ending in .mp4 implies --mp4. The path is printed on stdout\n"
        "when recording starts.\n"
        "\n"
        "Output:\n"
        "  --mp4       MP4 with AAC audio, which plays in web browsers and\n"
        "              chat apps (Discord). Only playable once recording is\n"
        "              stopped cleanly; an MKV cut short still plays\n"
        "\n"
        "Video:\n"
        "  -f FPS      frames per second, 1-120 (default 30)\n"
        "  -q CRF      x264 quality, 0 (lossless) - 51 (default 23)\n"
        "  -p PRESET   x264 preset (default ultrafast; slower presets give\n"
        "              smaller files for more CPU)\n"
        "  -j N        x264 threads (default 2)\n"
        "  -x          record at half width and height (a quarter of the work)\n"
        "  -K          encode every frame, even when the screen did not change\n"
        "  -d DEVICE   DRM device (default: first /dev/dri/cardN with a display)\n"
        "  -l          list the screens (CRTC ids, connector names), planes and\n"
        "              framebuffers, then exit\n"
        "  -c SCREENS  record only these screens, top to bottom: CRTC ids or\n"
        "              connector names from -l, comma-separated (RG-DS:\n"
        "              -c DSI-2 top screen, -c DSI-1 bottom screen)\n"
        "  -S, --active  only the screen EmulationStation is on (RG-DS; read\n"
        "              from /var/run/drmConn); ignored with -c\n"
        "  -r          reverse the stacking order\n"
        "  --no-hw     capture and convert on the CPU even where hardware\n"
        "              could (Rockchip RGA, Allwinner display engine)\n"
        "  -R N        panel rotation 0-3, fbcon numbering (default: read from\n"
        "              /sys/class/graphics/fbcon/rotate); turned back upright\n"
        "Audio:\n"
        "  -a SOURCE   PulseAudio source (default @DEFAULT_MONITOR@, what the\n"
        "              speakers play)\n"
        "  -A          no audio\n"
        "  -B KBPS     Opus or AAC bitrate (default 128); 0 stores\n"
        "              uncompressed PCM (MKV only)\n"
        "Other:\n"
        "  -t SECONDS  stop after this long\n"
        "  -n NICE     CPU priority, -20 (highest) - 19 (default 10)\n"
        "  -v          verbose output and statistics on stderr\n"
        "  -h          show this help\n"
        "\n"
        "Modes (options after them override):\n"
        "  --default   -f 30 -q 23 -p ultrafast -j 2\n"
        "  --fast      -f 15 -q 26 -p ultrafast -j 1\n"
        "  --hq        -f 30 -q 20 -p ultrafast -j 2\n");
}

static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "default", no_argument, NULL, 1 },
        { "fast",    no_argument, NULL, 2 },
        { "hq",      no_argument, NULL, 3 },
        { "active",  no_argument, NULL, 'S' },
        { "no-hw",   no_argument, NULL, 4 },
        { "no-rga",  no_argument, NULL, 4 }, /* earlier name of --no-hw */
        { "mp4",     no_argument, NULL, 5 },
        { "help",    no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    /* --fast keeps up at 15 fps on one thread; a second only costs CPU. */
    static const struct { int fps, crf, threads; const char *preset; } modes[] = {
        { 0, 0, 0, NULL },
        { 30, 23, 2, "ultrafast" }, /* --default */
        { 15, 26, 1, "ultrafast" }, /* --fast */
        { 30, 20, 2, "ultrafast" }, /* --hq */
    };
    struct capture cap;
    struct audio aud;
    struct yuv frame;
    struct output out = { NULL, NULL };
    struct mkv_video mv;
    struct mkv_audio ma;
    struct mp4_video pv;
    struct mp4_audio pa4;
    struct rect all;
    x264_param_t prm;
    x264_picture_t pic;
    x264_t *enc = NULL;
    struct sigaction sa;
    uint8_t avcc[512];
    struct screen_pick want[MAX_SCREENS];
    char path[PATH_MAX + 64], prefix[PATH_MAX], stamp[32] = "";
    const char *outarg = NULL;
    const char *dev = NULL, *source = "@DEFAULT_MONITOR@", *preset = "ultrafast";
    int fps = 30, crf = 23, threads = 2, half = 0, every = 0, rot = -1;
    int nwant = 0, reverse = 0, active = 0, norga = 0, audio = 1, kbps = 128, niceness = 10, secs = 0;
    int mp4 = 0, list = 0;
    const char *ext;
    int opt, i, w, h, have_audio = 0, ret = 0, idle_told = 0;
    int64_t idle_since = -1;  /* display engine idle since (ms), or -1 */
    uint8_t *picbuf = NULL;
    size_t picsize;
    int64_t t0, next, interval, dur, last_ts = -1;
    int64_t frames = 0, skipped = 0, dropped = 0, vbytes = 0;
    int64_t t_cap = 0, t_enc = 0;

    prog_name = "knulli-record";
    memset(&cap, 0, sizeof(cap));
    memset(&aud, 0, sizeof(aud));
    memset(&ma, 0, sizeof(ma));
    cap.fd = cap.fbfd = -1;

    while ((opt = getopt_long(argc, argv, "f:q:p:j:xKd:c:SrR:a:AB:t:n:lvh",
                              longopts, NULL)) != -1) {
        switch (opt) {
        case 1: case 2: case 3:
            fps = modes[opt].fps;
            crf = modes[opt].crf;
            threads = modes[opt].threads;
            preset = modes[opt].preset;
            break;
        case 'f':
            if (parse_int(optarg, 1, 120, &fps)) {
                err("frame rate must be 1-120\n");
                return 2;
            }
            break;
        case 'q':
            if (parse_int(optarg, 0, 51, &crf)) {
                err("quality must be 0-51\n");
                return 2;
            }
            break;
        case 'p': preset = optarg; break;
        case 'j':
            if (parse_int(optarg, 1, 16, &threads)) {
                err("threads must be 1-16\n");
                return 2;
            }
            break;
        case 'x': half = 1; break;
        case 'K': every = 1; break;
        case 'd': dev = optarg; break;
        case 'c':
            if (parse_crtc_list(optarg, want, &nwant) != 0)
                return 2;
            break;
        case 'S': active = 1; break;
        case 4: norga = 1; break;
        case 5: mp4 = 1; break;
        case 'r': reverse = 1; break;
        case 'R':
            if (parse_int(optarg, 0, 3, &rot)) {
                err("rotation must be 0-3\n");
                return 2;
            }
            break;
        case 'a': source = optarg; break;
        case 'A': audio = 0; break;
        case 'B':
            if (parse_int(optarg, 0, 512, &kbps) || (kbps > 0 && kbps < 6)) {
                err("audio bitrate must be 6-512 kbit/s, or 0 for PCM\n");
                return 2;
            }
            break;
        case 't':
            if (parse_int(optarg, 1, INT_MAX, &secs)) {
                err("invalid duration\n");
                return 2;
            }
            break;
        case 'n':
            if (parse_int(optarg, -20, 19, &niceness)) {
                err("nice must be -20 - 19\n");
                return 2;
            }
            break;
        case 'l': list = 1; break;
        case 'v': verbose = 1; break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (list) {
        int fd = drm_open(dev);

        if (fd >= 0) {
            drm_list(fd);
            close(fd);
        } else if (dev || fbdev_list() != 0) {
            err("no usable display found\n");
            return 1;
        }
        return 0;
    }

    /* The old script took an encoder name ("software"); accept and ignore it. */
    for (i = optind; i < argc; i++) {
        if (!strcmp(argv[i], "software"))
            continue;
        if (outarg) {
            usage(stderr);
            return 2;
        }
        outarg = argv[i];
    }
    if (outarg && strlen(outarg) > 4 &&
        strcasecmp(outarg + strlen(outarg) - 4, ".mp4") == 0)
        mp4 = 1;
    if (mp4 && audio && kbps == 0) {
        err("MP4 files take AAC audio; -B 0 (PCM) is for MKV only\n");
        return 2;
    }
    ext = mp4 ? "mp4" : "mkv";

    /* Leave the CPU and the SD card to the game first. Threads created from
     * here on (audio, x264) inherit this. */
    if (setpriority(PRIO_PROCESS, 0, niceness) != 0)
        logv("setpriority: %s\n", strerror(errno));
#ifdef SYS_ioprio_set
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0,
            3 << 13 /* IOPRIO_CLASS_IDLE */);
#endif

    if (capture_open(&cap, dev, want, nwant, reverse, active) != 0) {
        capture_close(&cap);
        return 1;
    }
    if (rot < 0)
        rot = panel_rotation();
    cap.turns = (4 - rot) & 3;
    cap.half = half;
    layout(&cap, &w, &h);
    if (w < 16 || h < 16) {
        err("picture too small (%dx%d)\n", w, h);
        capture_close(&cap);
        return 1;
    }
    if (cap.turns)
        logv("panel rotation %d: turning the picture %d quarter turn(s) "
             "clockwise\n", rot, cap.turns);

    /* Output name: given path minus ".mkv" / ".mp4", or the default plus a
     * timestamp. */
    if (outarg && outarg[0]) {
        size_t len = strlen(outarg);

        if (len >= sizeof(prefix)) {
            err("path too long\n");
            capture_close(&cap);
            return 2;
        }
        strcpy(prefix, outarg);
        if (len > 4 && (strcasecmp(prefix + len - 4, ".mkv") == 0 ||
                        strcasecmp(prefix + len - 4, ".mp4") == 0))
            prefix[len - 4] = '\0';
    } else {
        time_t now = time(NULL);
        struct tm tm;

        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%Y%m%d_%Hh%Mm%Ss", &tm);
        snprintf(prefix, sizeof(prefix), "%s", DEFAULT_DIR "/" DEFAULT_PREFIX);
    }
    i = snprintf(path, sizeof(path), "%s_%04dx%d%s%s.%s", prefix, w, h,
                 *stamp ? "_" : "", stamp, ext);
    if (i < 0 || (size_t)i >= sizeof(path)) {
        err("path too long\n");
        capture_close(&cap);
        return 2;
    }
    make_parent_dirs(path);

    /* H.264: fast, no lookahead or B-frames, timestamps from the clock. */
    if (x264_param_default_preset(&prm, preset, "zerolatency") < 0) {
        err("unknown x264 preset '%s'\n", preset);
        capture_close(&cap);
        return 2;
    }
    prm.i_width = w;
    prm.i_height = h;
    prm.i_csp = X264_CSP_I420;
    prm.i_threads = threads;
    /* zerolatency splits each frame between the threads (sliced threads);
     * on these cores giving each thread its own frame scales much better
     * (1.7x vs 1.3x for 2 threads on the H700) and lets the next capture
     * overlap the encode. It holds back a frame or so, which is no matter
     * here: the rest of zerolatency (no lookahead, no B-frames) stays. */
    prm.b_sliced_threads = 0;
    prm.i_fps_num = fps;
    prm.i_fps_den = 1;
    prm.b_vfr_input = 1;
    prm.i_timebase_num = 1;
    prm.i_timebase_den = 1000;
    prm.i_keyint_max = fps * 2;
    prm.b_repeat_headers = 0;
    prm.b_annexb = 0;
    prm.rc.i_rc_method = X264_RC_CRF;
    prm.rc.f_rf_constant = (float)crf;
    prm.vui.i_colorprim = 6; /* SMPTE 170M: BT.601, as converted */
    prm.vui.i_transfer = 6;
    prm.vui.i_colmatrix = 6;
    prm.vui.b_fullrange = 0;
    prm.i_log_level = verbose ? X264_LOG_WARNING : X264_LOG_ERROR;
    enc = x264_encoder_open(&prm);
    logv("video: %dx%d, %d fps, x264 %s crf %d, %d thread%s\n", w, h, fps,
         preset, crf, threads, threads == 1 ? "" : "s");
    /* The picture lives in page-aligned memory the RGA can write into. */
    picsize = ((size_t)w * h * 3 / 2 + 4095) & ~(size_t)4095;
    picbuf = enc ? aligned_alloc(4096, picsize) : NULL;
    if (!enc || !picbuf) {
        err("cannot start the H.264 encoder\n");
        ret = 1;
        goto out;
    }
    x264_picture_init(&pic);
    pic.img.i_csp = X264_CSP_I420;
    pic.img.i_plane = 3;
    pic.img.plane[0] = picbuf;
    pic.img.plane[1] = picbuf + (size_t)w * h;
    pic.img.plane[2] = picbuf + (size_t)w * h * 5 / 4;
    pic.img.i_stride[0] = w;
    pic.img.i_stride[1] = pic.img.i_stride[2] = w / 2;
    if (!norga && cap.fd >= 0 && rga_open() == 0) {
        cap.rga_pic = rga_import_mem(picbuf, w, h, rga_i420());
        cap.use_rga = cap.rga_pic != 0;
        cap.pic_w = w;
        cap.pic_h = h;
    }
    /* Allwinner: let the display engine capture the screen, at the
     * upright size of the picture (turned afterwards on the CPU). */
    if (!norga && cap.fd < 0) {
        int sw, sh;

        if (sunxi_screen_size(&sw, &sh) == 0 &&
            sunxi_open(&cap.sx, cap.slots[0].uw, cap.slots[0].uh) == 0)
            cap.use_sx = 1;
    }
    logv("conversion: %s\n", cap.use_rga ? "RGA" :
         cap.use_sx ? "display engine" : "CPU");
    frame.w = w;
    frame.h = h;
    for (i = 0; i < 3; i++) {
        frame.p[i] = pic.img.plane[i];
        frame.stride[i] = pic.img.i_stride[i];
    }
    all.x = all.y = 0;
    all.w = w;
    all.h = h;
    yuv_fill_black(&frame, &all);

    memset(&mv, 0, sizeof(mv));
    mv.width = w;
    mv.height = h;
    mv.avcc = avcc;
    mv.avcc_len = make_avcc(enc, avcc, sizeof(avcc));
    if (!mv.avcc_len) {
        err("cannot get the H.264 headers\n");
        ret = 1;
        goto out;
    }

    queue_init();
    /* Started from a hotkey there is no XDG_RUNTIME_DIR; point libpulse at
     * Knulli's system socket rather than have it try (and complain about)
     * a runtime-dir symlink on the FAT /userdata. */
    if (!getenv("PULSE_SERVER") && !getenv("PULSE_RUNTIME_PATH") &&
        !getenv("XDG_RUNTIME_DIR") && access("/var/run/pulse/native", F_OK) == 0)
        setenv("PULSE_RUNTIME_PATH", "/var/run/pulse", 0);
    if (audio && audio_open(&aud, source, kbps, mp4) == 0) {
        have_audio = 1;
        ma.rate = AUDIO_RATE;
        ma.channels = AUDIO_CH;
        ma.opus = aud.opus != NULL;
        ma.head = aud.head;
        ma.head_len = sizeof(aud.head);
        ma.delay_ns = (uint64_t)aud.preskip * 1000000000ULL / AUDIO_RATE;
    }

    if (mp4) {
        pv.width = w;
        pv.height = h;
        pv.avcc = mv.avcc;
        pv.avcc_len = mv.avcc_len;
        memset(&pa4, 0, sizeof(pa4));
        pa4.rate = AUDIO_RATE;
        pa4.channels = AUDIO_CH;
        pa4.frame = AAC_FRAME;
        pa4.delay = AAC_DELAY;
        pa4.bitrate = aud.bitrate;
        pa4.asc = aud.asc;
        pa4.asc_len = sizeof(aud.asc);
        out.mp4 = mp4_open(path, &pv, have_audio ? &pa4 : NULL);
    } else {
        out.mkv = mkv_open(path, &mv, have_audio ? &ma : NULL);
    }
    if (!out.mp4 && !out.mkv) {
        err("%s: %s\n", path, strerror(errno));
        ret = 1;
        goto out;
    }
    printf("%s\n", path);
    fflush(stdout);
    if (isatty(2))
        fprintf(stderr, "Recording %dx%d at %d fps%s. Press Ctrl+C to stop.\n",
                w, h, fps, have_audio ? " with sound" : "");

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    t0 = now_ns();
    if (have_audio && audio_start(&aud, t0) != 0)
        err("audio: cannot start the audio thread\n");

    interval = 1000000000LL / fps;
    next = t0;
    while (!stop) {
        struct timespec ts;
        int64_t now, ms, a, b;
        int changed;

        ts.tv_sec = next / 1000000000;
        ts.tv_nsec = next % 1000000000;
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
            continue;
        now = now_ns();
        /* Stamp frames with their tick, not the moment we got to them, so
         * they sit on an exact 1/fps grid (gaps where skipped). */
        ms = (next - t0) / 1000000;
        if (secs && ms >= (int64_t)secs * 1000)
            break;

        a = now_ns();
        changed = cap.fd >= 0 ? drm_poll(&cap) : 1;
        if (changed || every || last_ts < 0 || ms - last_ts >= KEEPALIVE_MS) {
            int v = verbose, drew = -1;
            int64_t fms = ms;     /* when the picture was taken */
            int64_t missed = cap.sx_missed;

            verbose = frames ? 0 : v; /* buffer details for the first frame only */
            if (cap.use_sx) {
                drew = sunxi_draw(&cap, &frame, ms, &fms);
                /* Report idle spells of a second or more (ES's menu only
                 * flips while something moves). */
                if (cap.sx_missed != missed) {
                    if (idle_since < 0)
                        idle_since = ms;
                    if (v && !idle_told && ms - idle_since >= 1000) {
                        fprintf(stderr, "%.1f s: display engine idle (no page "
                                "flips), reading the framebuffer\n",
                                idle_since / 1000.0);
                        idle_told = 1;
                    }
                } else if (drew == 0) {
                    if (idle_told)
                        fprintf(stderr, "%.1f s: display engine capturing "
                                "again\n", ms / 1000.0);
                    idle_since = -1;
                    idle_told = 0;
                }
            }
            if (drew < 0)
                drew = cap.fd >= 0 ? drm_draw(&cap, &frame, now,
                                              !changed || every)
                                   : fbdev_draw(&cap, &frame);
            verbose = v;
            if (drew == 0 && fms > last_ts) {
                b = now_ns();
                t_cap += b - a;
                pic.i_pts = fms;
                pic.i_type = X264_TYPE_AUTO;
                if (encode(enc, &pic, &vbytes) < 0) {
                    err("H.264 encoding failed\n");
                    ret = 1;
                    break;
                }
                t_enc += now_ns() - b;
                last_ts = fms;
                frames++;
            }
        } else {
            skipped++;
        }
        if (cap.fd >= 0)
            expire_maps(&cap, now);
        if (queue_drain(&out, ms - MUX_DELAY_MS) != 0) {
            err("%s: write failed\n", path);
            ret = 1;
            break;
        }

        /* Next tick; ticks we are too late for are dropped, not queued. */
        next += interval;
        now = now_ns();
        if (next <= now) {
            int64_t late = (now - next) / interval + 1;

            dropped += late;
            next += late * interval;
        }
    }
    stop = 1;
    dur = (now_ns() - t0) / 1000000;

    /* Frames x264 still holds (none with zerolatency, but to be sure). */
    while (x264_encoder_delayed_frames(enc) > 0 &&
           encode(enc, NULL, &vbytes) > 0)
        ;
    if (have_audio)
        audio_close(&aud);
    queue_drain(&out, INT64_MAX);
    if (output_close(&out, dur) != 0) {
        err("%s: write failed\n", path);
        ret = 1;
    }

    if (verbose) {
        struct rusage ru;
        double cpu;

        getrusage(RUSAGE_SELF, &ru);
        cpu = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec +
              (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
        fprintf(stderr,
                "%.1f s: %lld frames encoded, %lld unchanged skipped, "
                "%lld dropped (late)\n", dur / 1000.0, (long long)frames,
                (long long)skipped, (long long)dropped);
        if (frames)
            fprintf(stderr, "capture %.2f ms/frame (%s), encode %.2f ms/frame, "
                    "video %lld kbit/s\n", t_cap / 1e6 / frames,
                    cap.rga_frames ? "RGA" : cap.sx_frames ? "display engine" :
                    "CPU", t_enc / 1e6 / frames,
                    (long long)(dur ? vbytes * 8 / dur : 0));
        if (cap.sx_missed)
            fprintf(stderr, "%lld frames read by CPU (display engine idle)\n",
                    (long long)cap.sx_missed);
        fprintf(stderr, "audio %lld packets, %lld resyncs; CPU %.1f%% of one "
                "core\n", (long long)aud.packets, (long long)aud.resyncs,
                dur ? 100.0 * cpu / (dur / 1000.0) : 0.0);
    }

out:
    output_close(&out, 0);
    if (have_audio)
        audio_close(&aud);
    rga_release(cap.rga_pic);
    free(picbuf);
    if (enc)
        x264_encoder_close(enc);
    capture_close(&cap);
    return ret;
}
