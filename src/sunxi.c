/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sunxi.c - Allwinner display-engine capture ("write-back") for
 * knulli-record.
 *
 * On the H700 / H616 the framebuffer is uncached and reading it costs the
 * CPU ~20 ms per 720x720 frame. The display engine can instead write the
 * finished screen -- every layer composited, scaled if asked -- into memory
 * as YUV 4:2:0 at the next vsync. The buffers come from ION's CMA heap
 * (cached; synced for the CPU through dma-buf), so reading one back costs
 * ~1.7 ms. Measured on the RG CubeXX (Knulli H700 kernel, 4.9).
 *
 * Needs the vendor display driver (/dev/disp) and ION (/dev/ion, staging
 * 4.9 API). The A133P TrimUI Hammer's 5.15 kernel has neither ION nor
 * dma-heaps, so there is no buffer the engine can write and the CPU can
 * read; the recorder falls back to reading /dev/fb0 there.
 *
 * The engine only carries out a queued capture when its register queue
 * (RCQ) runs, i.e. when some program commits a new layer setup -- as
 * EmulationStation does with every page flip. A program that draws into
 * the page on screen without flipping (knulli-gamepad's OSD) leaves the
 * captures undone, and the buffers keep their old pictures. So each
 * buffer gets a marker written into its last bytes before it is queued;
 * if the marker is still there when the buffer comes back, the engine did
 * not write it, and sunxi_step() says so instead of handing it out.
 *
 * Definitions are copied from the 4.9 kernel's include/video/
 * sunxi_display2.h and drivers/staging/android/uapi/ion.h.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "kms.h"
#include "sunxi.h"

/* ---- sunxi_display2.h ---------------------------------------------------- */
struct disp_rect { int x, y; unsigned int width, height; };
struct disp_rectsz { unsigned int width, height; };
struct disp_s_frame2 {
    int format;
    struct disp_rectsz size[3];
    struct disp_rect crop;
    int fd;
};
struct disp_capture_info2 {
    struct disp_rect window;      /* all zero: the whole screen */
    struct disp_s_frame2 out_frame;
};
#define DISP_GET_SCN_WIDTH    0x07
#define DISP_GET_SCN_HEIGHT   0x08
#define DISP_CAPTURE_START    0x140
#define DISP_CAPTURE_STOP     0x141
#define DISP_CAPTURE_COMMIT2  0x143
#define DISP_FORMAT_YUV420_P  0x48

/* ---- ion.h (staging, 4.9: handle based) -------------------------------- */
struct ion_allocation_data {
    size_t len, align;
    unsigned int heap_id_mask, flags;
    int handle;
};
struct ion_fd_data { int handle, fd; };
struct ion_handle_data { int handle; };
#define ION_IOC_ALLOC   _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE    _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_SHARE   _IOWR('I', 4, struct ion_fd_data)
#define ION_FLAG_CACHED 1
#define ION_CMA_HEAP    4         /* "cma" in the sun50iw9 device tree */

/* ---- dma-buf.h (not in the 4.9 sysroot headers) ------------------------- */
struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ   (1 << 0)
#define DMA_BUF_SYNC_WRITE  (2 << 0)
#define DMA_BUF_SYNC_START  (0 << 2)
#define DMA_BUF_SYNC_END    (1 << 2)
#define DMA_BUF_IOCTL_SYNC  _IOW('b', 0, struct dma_buf_sync)

/* The driver takes a pointer to four longs: screen, argument, ... */
static int disp_ioctl(int fd, unsigned long cmd, unsigned long a0,
                      unsigned long a1)
{
    unsigned long args[4] = { a0, a1, 0, 0 };

    return ioctl(fd, cmd, args);
}

int sunxi_screen_size(int *w, int *h)
{
    int fd = open("/dev/disp", O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return -1;
    *w = disp_ioctl(fd, DISP_GET_SCN_WIDTH, 0, 0);
    *h = disp_ioctl(fd, DISP_GET_SCN_HEIGHT, 0, 0);
    close(fd);
    return *w > 0 && *h > 0 ? 0 : -1;
}

static void buf_free(struct sunxi_cap *s, int i)
{
    struct ion_handle_data hd;

    if (s->buf[i].map)
        munmap(s->buf[i].map, s->buf[i].len);
    if (s->buf[i].fd >= 0)
        close(s->buf[i].fd);
    if (s->buf[i].handle) {
        hd.handle = s->buf[i].handle;
        ioctl(s->ion, ION_IOC_FREE, &hd);
    }
    s->buf[i].map = NULL;
    s->buf[i].fd = -1;
    s->buf[i].handle = 0;
}

static void dmabuf_cpu(int fd, uint64_t rw, int end)
{
    struct dma_buf_sync sy;

    sy.flags = rw | (end ? DMA_BUF_SYNC_END : DMA_BUF_SYNC_START);
    xioctl(fd, DMA_BUF_IOCTL_SYNC, &sy);
}

/* The marker: the last MARK_WORDS words of the picture (bottom right of the
 * U plane), a pattern no capture leaves behind. */
#define MARK_WORDS 4

static uint64_t *mark_at(struct sunxi_cap *s, int i)
{
    return (uint64_t *)(s->buf[i].map + (size_t)s->w * s->h * 3 / 2) -
           MARK_WORDS;
}

static void mark_word(uint64_t seq, int k, uint64_t *v)
{
    *v = (0x6b6e756c6c69ull ^ seq * 0x9e3779b97f4a7c15ull) + (uint64_t)k;
}

/* Write buffer i's marker and push it out of the CPU cache. */
static void mark(struct sunxi_cap *s, int i)
{
    uint64_t *m = mark_at(s, i);
    int k;

    s->buf[i].seq = ++s->seq;
    dmabuf_cpu(s->buf[i].fd, DMA_BUF_SYNC_WRITE, 0);
    for (k = 0; k < MARK_WORDS; k++)
        mark_word(s->seq, k, &m[k]);
    dmabuf_cpu(s->buf[i].fd, DMA_BUF_SYNC_WRITE, 1);
}

/* Buffer i still has its marker: the engine has not written it. */
static int marked(struct sunxi_cap *s, int i)
{
    const uint64_t *m = mark_at(s, i);
    uint64_t v;
    int k;

    for (k = 0; k < MARK_WORDS; k++) {
        mark_word(s->buf[i].seq, k, &v);
        if (m[k] != v)
            return 0;
    }
    return 1;
}

int sunxi_open(struct sunxi_cap *s, int w, int h)
{
    int i;

    memset(s, 0, sizeof(*s));
    s->held = -1;
    for (i = 0; i < SUNXI_BUFFERS; i++)
        s->buf[i].fd = -1;
    s->w = w;
    s->h = h;
    s->disp = open("/dev/disp", O_RDWR | O_CLOEXEC);
    s->ion = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (s->disp < 0 || s->ion < 0)
        goto fail;

    for (i = 0; i < SUNXI_BUFFERS; i++) {
        size_t len = ((size_t)w * h * 3 / 2 + 4095) & ~(size_t)4095;
        struct ion_allocation_data a = { len, 4096, 1u << ION_CMA_HEAP,
                                         ION_FLAG_CACHED, 0 };
        struct ion_fd_data sh;
        void *map;

        if (xioctl(s->ion, ION_IOC_ALLOC, &a) != 0) {
            logv("display capture: ION alloc: %s\n", strerror(errno));
            goto fail;
        }
        s->buf[i].handle = a.handle;
        sh.handle = a.handle;
        if (xioctl(s->ion, ION_IOC_SHARE, &sh) != 0)
            goto fail;
        s->buf[i].fd = sh.fd;
        map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, sh.fd, 0);
        if (map == MAP_FAILED)
            goto fail;
        s->buf[i].map = map;
        s->buf[i].len = len;
        s->buf[i].ms = -1;
    }
    if (disp_ioctl(s->disp, DISP_CAPTURE_START, 0, 0) != 0) {
        logv("display capture: cannot start\n");
        goto fail;
    }
    return 0;

fail:
    sunxi_close(s);
    return -1;
}

int sunxi_step(struct sunxi_cap *s, int64_t ms, struct yuv *out,
               int64_t *out_ms)
{
    struct disp_capture_info2 ci;
    int i = s->next, ready = (s->next + 1) % SUNXI_BUFFERS;

    /*
     * The engine takes a request at the next vsync and writes the frame
     * during the one after, so a capture is done within two 60 Hz frames;
     * with three buffers the one handed out was queued two ticks ago.
     */
    memset(&ci, 0, sizeof(ci));
    ci.out_frame.format = DISP_FORMAT_YUV420_P;
    ci.out_frame.size[0].width = (unsigned)s->w;
    ci.out_frame.size[0].height = (unsigned)s->h;
    ci.out_frame.size[1].width = (unsigned)s->w / 2;
    ci.out_frame.size[1].height = (unsigned)s->h / 2;
    ci.out_frame.size[2] = ci.out_frame.size[1];
    ci.out_frame.crop.width = (unsigned)s->w;
    ci.out_frame.crop.height = (unsigned)s->h;
    ci.out_frame.fd = s->buf[i].fd;
    mark(s, i);
    if (disp_ioctl(s->disp, DISP_CAPTURE_COMMIT2, 0, (unsigned long)&ci) != 0)
        return -1;
    s->buf[i].ms = ms;
    s->next = ready;

    if (s->buf[ready].ms < 0)
        return 0;                 /* the first two ticks */
    dmabuf_cpu(s->buf[ready].fd, DMA_BUF_SYNC_READ, 0);
    if (marked(s, ready)) {
        dmabuf_cpu(s->buf[ready].fd, DMA_BUF_SYNC_READ, 1);
        s->buf[ready].ms = -1;
        return 2;
    }
    s->held = ready;
    /* The engine writes the V plane before the U plane (YV12 order). */
    out->w = s->w;
    out->h = s->h;
    out->p[0] = s->buf[ready].map;
    out->p[2] = s->buf[ready].map + (size_t)s->w * s->h;
    out->p[1] = out->p[2] + (size_t)s->w * s->h / 4;
    out->stride[0] = s->w;
    out->stride[1] = out->stride[2] = s->w / 2;
    *out_ms = s->buf[ready].ms;
    s->buf[ready].ms = -1;
    return 1;
}

void sunxi_release(struct sunxi_cap *s)
{
    if (s->held >= 0)
        dmabuf_cpu(s->buf[s->held].fd, DMA_BUF_SYNC_READ, 1);
    s->held = -1;
}

void sunxi_close(struct sunxi_cap *s)
{
    int i;

    sunxi_release(s);
    if (s->disp >= 0)
        disp_ioctl(s->disp, DISP_CAPTURE_STOP, 0, 0);
    for (i = 0; i < SUNXI_BUFFERS; i++)
        buf_free(s, i);
    if (s->ion > 0)
        close(s->ion);
    if (s->disp > 0)
        close(s->disp);
    s->ion = s->disp = -1;
}
