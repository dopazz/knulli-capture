/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mkv.c - minimal Matroska writer for knulli-record.
 *
 * Clusters are built in memory and written whole, each starting at a video
 * keyframe, with the segment size left "unknown" until the end. A recording
 * cut short (power off, kill -9) therefore still plays up to the last
 * cluster; a clean close adds the cue index, the duration and the seek head
 * so players can seek.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mkv.h"

/* Element ids */
#define ID_EBML              0x1A45DFA3
#define ID_EBMLVERSION       0x4286
#define ID_EBMLREADVERSION   0x42F7
#define ID_EBMLMAXIDLENGTH   0x42F2
#define ID_EBMLMAXSIZELENGTH 0x42F3
#define ID_DOCTYPE           0x4282
#define ID_DOCTYPEVERSION    0x4287
#define ID_DOCTYPEREADVER    0x4285
#define ID_SEGMENT           0x18538067
#define ID_SEEKHEAD          0x114D9B74
#define ID_SEEK              0x4DBB
#define ID_SEEKID            0x53AB
#define ID_SEEKPOSITION      0x53AC
#define ID_INFO              0x1549A966
#define ID_TIMESTAMPSCALE    0x2AD7B1
#define ID_DURATION          0x4489
#define ID_MUXINGAPP         0x4D80
#define ID_WRITINGAPP        0x5741
#define ID_TRACKS            0x1654AE6B
#define ID_TRACKENTRY        0xAE
#define ID_TRACKNUMBER       0xD7
#define ID_TRACKUID          0x73C5
#define ID_TRACKTYPE         0x83
#define ID_FLAGLACING        0x9C
#define ID_CODECID           0x86
#define ID_CODECPRIVATE      0x63A2
#define ID_CODECDELAY        0x56AA
#define ID_SEEKPREROLL       0x56BB
#define ID_VIDEO             0xE0
#define ID_PIXELWIDTH        0xB0
#define ID_PIXELHEIGHT       0xBA
#define ID_AUDIO             0xE1
#define ID_SAMPLINGFREQ      0xB5
#define ID_CHANNELS          0x9F
#define ID_BITDEPTH          0x6264
#define ID_CLUSTER           0x1F43B675
#define ID_TIMESTAMP         0xE7
#define ID_SIMPLEBLOCK       0xA3
#define ID_CUES              0x1C53BB6B
#define ID_CUEPOINT          0xBB
#define ID_CUETIME           0xB3
#define ID_CUETRACKPOSITIONS 0xB7
#define ID_CUETRACK          0xF7
#define ID_CUECLUSTERPOS     0xF1
#define ID_VOID              0xEC

#define SEEKHEAD_SPACE  96    /* reserved up front for the seek head */
#define CLUSTER_MAX_MS  5000  /* also start a cluster this often */

struct buf {
    uint8_t *d;
    size_t n, cap;
    int oom;
};

struct cue {
    int64_t ts;
    uint64_t pos;
};

struct mkv {
    FILE *f;
    uint64_t seg_start;     /* file offset of the segment's payload */
    uint64_t seekhead_pos;  /* file offset of the reserved space */
    uint64_t duration_pos;  /* file offset of the Duration float */
    uint64_t info_pos, tracks_pos;
    struct buf cl;          /* cluster being built (payload) */
    int64_t cl_ts;          /* its timestamp */
    int cl_open;
    int cl_key;             /* starts with a video keyframe */
    struct cue *cues;
    size_t ncues, cuecap;
    int err;
};

static void need(struct buf *b, size_t n)
{
    size_t cap;
    uint8_t *d;

    if (b->n + n <= b->cap || b->oom)
        return;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + n)
        cap *= 2;
    d = realloc(b->d, cap);
    if (!d) {
        b->oom = 1;
        return;
    }
    b->d = d;
    b->cap = cap;
}

static void put(struct buf *b, const void *p, size_t n)
{
    need(b, n);
    if (b->oom)
        return;
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void put_byte(struct buf *b, uint8_t v)
{
    put(b, &v, 1);
}

static void put_id(struct buf *b, uint32_t id)
{
    int i;

    for (i = 3; i > 0 && !(id >> (8 * i)); i--)
        ;
    for (; i >= 0; i--)
        put_byte(b, (uint8_t)(id >> (8 * i)));
}

/* EBML variable-size integer, in `len` bytes (0: as few as possible). */
static void put_size(struct buf *b, uint64_t v, int len)
{
    int i;

    if (!len)
        for (len = 1; len < 8 && v >= (1ULL << (7 * len)) - 1; len++)
            ;
    v |= 1ULL << (7 * len);
    for (i = len - 1; i >= 0; i--)
        put_byte(b, (uint8_t)(v >> (8 * i)));
}

static void put_uint(struct buf *b, uint32_t id, uint64_t v)
{
    int n, i;

    for (n = 1; n < 8 && (v >> (8 * n)); n++)
        ;
    put_id(b, id);
    put_size(b, n, 0);
    for (i = n - 1; i >= 0; i--)
        put_byte(b, (uint8_t)(v >> (8 * i)));
}

static void put_float(struct buf *b, uint32_t id, double v)
{
    uint64_t u;
    int i;

    memcpy(&u, &v, 8);
    put_id(b, id);
    put_size(b, 8, 0);
    for (i = 7; i >= 0; i--)
        put_byte(b, (uint8_t)(u >> (8 * i)));
}

static void put_bin(struct buf *b, uint32_t id, const void *p, size_t n)
{
    put_id(b, id);
    put_size(b, n, 0);
    put(b, p, n);
}

static void put_str(struct buf *b, uint32_t id, const char *s)
{
    put_bin(b, id, s, strlen(s));
}

/* A master element whose payload is `inner`. */
static void put_master(struct buf *b, uint32_t id, const struct buf *inner)
{
    put_id(b, id);
    put_size(b, inner->n, 0);
    put(b, inner->d, inner->n);
}

static void buf_free(struct buf *b)
{
    free(b->d);
    memset(b, 0, sizeof(*b));
}

static int write_buf(struct mkv *m, const struct buf *b)
{
    if (b->oom || fwrite(b->d, 1, b->n, m->f) != b->n)
        m->err = 1;
    return m->err ? -1 : 0;
}

static uint64_t tell(struct mkv *m)
{
    long long p = ftello(m->f);

    return p < 0 ? 0 : (uint64_t)p;
}

static void add_track(struct buf *tracks, const struct mkv_video *v,
                      const struct mkv_audio *a)
{
    struct buf t = { 0 }, sub = { 0 };

    if (v) {
        put_uint(&t, ID_TRACKNUMBER, MKV_VIDEO);
        put_uint(&t, ID_TRACKUID, MKV_VIDEO);
        put_uint(&t, ID_TRACKTYPE, 1);
        put_uint(&t, ID_FLAGLACING, 0);
        put_str(&t, ID_CODECID, "V_MPEG4/ISO/AVC");
        put_bin(&t, ID_CODECPRIVATE, v->avcc, v->avcc_len);
        put_uint(&sub, ID_PIXELWIDTH, v->width);
        put_uint(&sub, ID_PIXELHEIGHT, v->height);
        put_master(&t, ID_VIDEO, &sub);
    } else {
        put_uint(&t, ID_TRACKNUMBER, MKV_AUDIO);
        put_uint(&t, ID_TRACKUID, MKV_AUDIO);
        put_uint(&t, ID_TRACKTYPE, 2);
        put_uint(&t, ID_FLAGLACING, 0);
        if (a->opus) {
            put_str(&t, ID_CODECID, "A_OPUS");
            put_bin(&t, ID_CODECPRIVATE, a->head, a->head_len);
            put_uint(&t, ID_CODECDELAY, a->delay_ns);
            put_uint(&t, ID_SEEKPREROLL, 80000000);
        } else {
            put_str(&t, ID_CODECID, "A_PCM/INT/LIT");
        }
        put_float(&sub, ID_SAMPLINGFREQ, a->rate);
        put_uint(&sub, ID_CHANNELS, a->channels);
        if (!a->opus)
            put_uint(&sub, ID_BITDEPTH, 16);
        put_master(&t, ID_AUDIO, &sub);
    }
    put_master(tracks, ID_TRACKENTRY, &t);
    buf_free(&t);
    buf_free(&sub);
}

struct mkv *mkv_open(const char *path, const struct mkv_video *video,
                     const struct mkv_audio *audio)
{
    struct buf b = { 0 }, sub = { 0 };
    struct mkv *m = calloc(1, sizeof(*m));

    if (!m)
        return NULL;
    m->f = fopen(path, "wb");
    if (!m->f) {
        free(m);
        return NULL;
    }
    setvbuf(m->f, NULL, _IOFBF, 256 * 1024);

    put_uint(&sub, ID_EBMLVERSION, 1);
    put_uint(&sub, ID_EBMLREADVERSION, 1);
    put_uint(&sub, ID_EBMLMAXIDLENGTH, 4);
    put_uint(&sub, ID_EBMLMAXSIZELENGTH, 8);
    put_str(&sub, ID_DOCTYPE, "matroska");
    put_uint(&sub, ID_DOCTYPEVERSION, 4);
    put_uint(&sub, ID_DOCTYPEREADVER, 2);
    put_master(&b, ID_EBML, &sub);
    sub.n = 0;

    put_id(&b, ID_SEGMENT);
    put_size(&b, (1ULL << 56) - 1, 8); /* unknown until closed */
    m->seg_start = b.n;

    /* Room for the seek head, written on close. */
    m->seekhead_pos = b.n;
    put_id(&b, ID_VOID);
    put_size(&b, SEEKHEAD_SPACE - 2, 1);
    need(&b, SEEKHEAD_SPACE - 2);
    if (!b.oom) {
        memset(b.d + b.n, 0, SEEKHEAD_SPACE - 2);
        b.n += SEEKHEAD_SPACE - 2;
    }

    m->info_pos = b.n;
    put_uint(&sub, ID_TIMESTAMPSCALE, 1000000); /* timestamps in ms */
    put_str(&sub, ID_MUXINGAPP, "knulli-record");
    put_str(&sub, ID_WRITINGAPP, "knulli-record");
    put_float(&sub, ID_DURATION, 0.0);
    put_master(&b, ID_INFO, &sub);
    m->duration_pos = b.n - 8;
    sub.n = 0;

    m->tracks_pos = b.n;
    add_track(&sub, video, NULL);
    if (audio)
        add_track(&sub, NULL, audio);
    put_master(&b, ID_TRACKS, &sub);

    write_buf(m, &b);
    buf_free(&b);
    buf_free(&sub);
    if (m->err) {
        fclose(m->f);
        free(m);
        return NULL;
    }
    return m;
}

static int flush_cluster(struct mkv *m)
{
    struct buf b = { 0 };
    uint64_t pos;

    if (!m->cl_open)
        return 0;
    pos = tell(m) - m->seg_start;
    put_id(&b, ID_CLUSTER);
    put_size(&b, m->cl.n, 0);
    write_buf(m, &b);
    write_buf(m, &m->cl);
    buf_free(&b);

    if (m->cl_key) {
        if (m->ncues == m->cuecap) {
            size_t cap = m->cuecap ? 2 * m->cuecap : 256;
            struct cue *c = realloc(m->cues, cap * sizeof(*c));

            if (c) {
                m->cues = c;
                m->cuecap = cap;
            }
        }
        if (m->ncues < m->cuecap) {
            m->cues[m->ncues].ts = m->cl_ts;
            m->cues[m->ncues].pos = pos;
            m->ncues++;
        }
    }
    m->cl.n = 0;
    m->cl_open = 0;
    /* Hand finished clusters to the kernel so little is lost on a crash. */
    fflush(m->f);
    return m->err ? -1 : 0;
}

int mkv_write(struct mkv *m, int track, int64_t ts_ms, int keyframe,
              const uint8_t *data, size_t len)
{
    int64_t rel;

    if (m->cl_open) {
        rel = ts_ms - m->cl_ts;
        if ((track == MKV_VIDEO && keyframe) || rel > CLUSTER_MAX_MS ||
            rel < -32768 || rel > 32767)
            flush_cluster(m);
    }
    if (!m->cl_open) {
        m->cl_open = 1;
        m->cl_ts = ts_ms < 0 ? 0 : ts_ms;
        m->cl_key = track == MKV_VIDEO && keyframe;
        put_uint(&m->cl, ID_TIMESTAMP, (uint64_t)m->cl_ts);
    }
    rel = ts_ms - m->cl_ts;
    if (rel < -32768)
        rel = -32768;

    put_id(&m->cl, ID_SIMPLEBLOCK);
    put_size(&m->cl, len + 4, 0);
    put_size(&m->cl, (uint64_t)track, 1);
    put_byte(&m->cl, (uint8_t)((uint16_t)rel >> 8));
    put_byte(&m->cl, (uint8_t)rel);
    put_byte(&m->cl, keyframe ? 0x80 : 0x00);
    put(&m->cl, data, len);
    if (m->cl.oom)
        m->err = 1;
    return m->err ? -1 : 0;
}

static void put_seek(struct buf *b, uint32_t id, uint64_t pos)
{
    struct buf s = { 0 }, idb = { 0 };

    put_id(&idb, id);
    put_bin(&s, ID_SEEKID, idb.d, idb.n);
    put_uint(&s, ID_SEEKPOSITION, pos);
    put_master(b, ID_SEEK, &s);
    buf_free(&s);
    buf_free(&idb);
}

int mkv_close(struct mkv *m, int64_t duration_ms)
{
    struct buf b = { 0 }, sub = { 0 };
    uint64_t cues_pos = 0, end;
    size_t i;
    int ret;

    flush_cluster(m);

    if (m->ncues) {
        cues_pos = tell(m) - m->seg_start;
        for (i = 0; i < m->ncues; i++) {
            struct buf cp = { 0 }, tp = { 0 };

            put_uint(&tp, ID_CUETRACK, MKV_VIDEO);
            put_uint(&tp, ID_CUECLUSTERPOS, m->cues[i].pos);
            put_uint(&cp, ID_CUETIME, (uint64_t)m->cues[i].ts);
            put_master(&cp, ID_CUETRACKPOSITIONS, &tp);
            put_master(&sub, ID_CUEPOINT, &cp);
            buf_free(&cp);
            buf_free(&tp);
        }
        put_master(&b, ID_CUES, &sub);
        write_buf(m, &b);
        b.n = sub.n = 0;
    }
    end = tell(m);

    /* Seek head into the reserved space, padded out with a Void. */
    put_seek(&sub, ID_INFO, m->info_pos - m->seg_start);
    put_seek(&sub, ID_TRACKS, m->tracks_pos - m->seg_start);
    if (m->ncues)
        put_seek(&sub, ID_CUES, cues_pos);
    put_master(&b, ID_SEEKHEAD, &sub);
    if (b.n + 2 <= SEEKHEAD_SPACE) {
        put_id(&b, ID_VOID);
        put_size(&b, SEEKHEAD_SPACE - b.n - 1, 1);
        while (b.n < SEEKHEAD_SPACE)
            put_byte(&b, 0);
        if (!m->err && fseeko(m->f, (off_t)m->seekhead_pos, SEEK_SET) == 0)
            write_buf(m, &b);
    }
    b.n = 0;

    /* Duration (a big-endian double) and the segment's real size. */
    if (!m->err && fseeko(m->f, (off_t)m->duration_pos, SEEK_SET) == 0) {
        double d = (double)duration_ms;
        uint64_t u;

        memcpy(&u, &d, 8);
        for (i = 0; i < 8; i++)
            put_byte(&b, (uint8_t)(u >> (8 * (7 - i))));
        write_buf(m, &b);
        b.n = 0;
    }
    if (!m->err && fseeko(m->f, (off_t)m->seg_start - 8, SEEK_SET) == 0) {
        put_size(&b, end - m->seg_start, 8);
        write_buf(m, &b);
    }

    if (fflush(m->f) != 0)
        m->err = 1;
    fsync(fileno(m->f));
    ret = (fclose(m->f) != 0 || m->err) ? -1 : 0;
    buf_free(&b);
    buf_free(&sub);
    buf_free(&m->cl);
    free(m->cues);
    free(m);
    return ret;
}
