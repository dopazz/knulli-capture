/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * kms.c - display capture shared by knulli-screenshot and knulli-record.
 *
 *   DRM/KMS: active CRTCs (screens), their planes and framebuffers. A
 *   framebuffer is mapped as dma-buf, or as a dumb buffer as a fallback.
 *   A screen is rebuilt from all its planes -- primary, overlays such as the
 *   RG-DS on-screen keyboard, cursor -- stacked by zpos and blended at their
 *   position, scale and alpha.
 *   fbdev: /dev/fb0 when DRM capture is not possible.
 *   Panel rotation as fbcon reports it, and RGB image helpers.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#if defined(__has_include)
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#endif
#endif

#include "kms.h"

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

const char *prog_name = "knulli";
int verbose;
int quiet;

void logv(const char *fmt, ...)
{
    va_list ap;

    if (!verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void err(const char *fmt, ...)
{
    va_list ap;

    if (quiet && !verbose)
        return;
    fprintf(stderr, "%s: ", prog_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

int xioctl(int fd, unsigned long req, void *arg)
{
    int r;

    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* ------------------------------------------------------------------------ */
/* Pixel formats                                                            */
/* ------------------------------------------------------------------------ */


static const struct {
    uint32_t fourcc;
    struct pixfmt fmt;
} drm_formats[] = {
    { DRM_FORMAT_XRGB8888,    { 32, 16, 8, 8, 8, 0, 8, 0, 0 } },
    { DRM_FORMAT_ARGB8888,    { 32, 16, 8, 8, 8, 0, 8, 24, 8 } },
    { DRM_FORMAT_XBGR8888,    { 32, 0, 8, 8, 8, 16, 8, 0, 0 } },
    { DRM_FORMAT_ABGR8888,    { 32, 0, 8, 8, 8, 16, 8, 24, 8 } },
    { DRM_FORMAT_RGBX8888,    { 32, 24, 8, 16, 8, 8, 8, 0, 0 } },
    { DRM_FORMAT_RGBA8888,    { 32, 24, 8, 16, 8, 8, 8, 0, 8 } },
    { DRM_FORMAT_BGRX8888,    { 32, 8, 8, 16, 8, 24, 8, 0, 0 } },
    { DRM_FORMAT_BGRA8888,    { 32, 8, 8, 16, 8, 24, 8, 0, 8 } },
    { DRM_FORMAT_RGB888,      { 24, 16, 8, 8, 8, 0, 8, 0, 0 } },
    { DRM_FORMAT_BGR888,      { 24, 0, 8, 8, 8, 16, 8, 0, 0 } },
    { DRM_FORMAT_RGB565,      { 16, 11, 5, 5, 6, 0, 5, 0, 0 } },
    { DRM_FORMAT_BGR565,      { 16, 0, 5, 5, 6, 11, 5, 0, 0 } },
    { DRM_FORMAT_XRGB1555,    { 16, 10, 5, 5, 5, 0, 5, 0, 0 } },
    { DRM_FORMAT_ARGB1555,    { 16, 10, 5, 5, 5, 0, 5, 15, 1 } },
    { DRM_FORMAT_XRGB2101010, { 32, 20, 10, 10, 10, 0, 10, 0, 0 } },
    { DRM_FORMAT_ARGB2101010, { 32, 20, 10, 10, 10, 0, 10, 30, 2 } },
    { DRM_FORMAT_XBGR2101010, { 32, 0, 10, 10, 10, 20, 10, 0, 0 } },
    { DRM_FORMAT_ABGR2101010, { 32, 0, 10, 10, 10, 20, 10, 30, 2 } },
};

const struct pixfmt pixfmt_rgb24 = { 24, 0, 8, 8, 8, 16, 8, 0, 0 };

const struct pixfmt *lookup_fourcc(uint32_t fourcc)
{
    size_t i;

    for (i = 0; i < sizeof(drm_formats) / sizeof(drm_formats[0]); i++)
        if (drm_formats[i].fourcc == fourcc)
            return &drm_formats[i].fmt;
    return NULL;
}

const char *fourcc_str(uint32_t fourcc, char buf[5])
{
    int i;

    for (i = 0; i < 4; i++) {
        char c = (char)(fourcc >> (8 * i));
        buf[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    buf[4] = '\0';
    return buf;
}

/*
 * Convert w x h pixels (rows `pitch` bytes apart) to packed RGB24, and the
 * alpha channel to `alpha` when that is non-NULL.
 */
static void convert(const uint8_t *src, size_t pitch, uint32_t w, uint32_t h,
                    const struct pixfmt *fmt, uint8_t *dst, uint8_t *alpha)
{
    int bytes = fmt->bpp / 8;
    uint32_t x, y;
    int i;

    for (y = 0; y < h; y++) {
        const uint8_t *p = src + (size_t)y * pitch;

        for (x = 0; x < w; x++, p += bytes) {
            uint32_t v = 0;

            for (i = 0; i < bytes; i++)
                v |= (uint32_t)p[i] << (8 * i);
            *dst++ = extract(v, fmt->r_off, fmt->r_len);
            *dst++ = extract(v, fmt->g_off, fmt->g_len);
            *dst++ = extract(v, fmt->b_off, fmt->b_len);
            if (alpha)
                *alpha++ = extract(v, fmt->a_off, fmt->a_len);
        }
    }
}

/*
 * Copy rows out of a (possibly uncached / write-combined) mapping into normal
 * memory first, then convert. This keeps the time spent reading the live
 * scanout buffer short and avoids slow byte-wise reads from device memory.
 */
static int image_from_mapping(const uint8_t *base, size_t pitch, uint32_t w,
                              uint32_t h, const struct pixfmt *fmt,
                              struct image *img, int want_alpha)
{
    size_t rowbytes = (size_t)w * (fmt->bpp / 8);
    int alpha = want_alpha && fmt->a_len;
    uint8_t *raw;
    uint32_t y;

    raw = malloc(rowbytes * h);
    img->rgb = malloc((size_t)w * h * 3);
    img->alpha = alpha ? malloc((size_t)w * h) : NULL;
    if (!raw || !img->rgb || (alpha && !img->alpha)) {
        free(raw);
        free(img->rgb);
        free(img->alpha);
        img->rgb = img->alpha = NULL;
        err("out of memory\n");
        return -1;
    }
    for (y = 0; y < h; y++)
        memcpy(raw + y * rowbytes, base + y * pitch, rowbytes);

    convert(raw, rowbytes, w, h, fmt, img->rgb, img->alpha);
    free(raw);
    img->w = w;
    img->h = h;
    return 0;
}

void free_images(struct image *imgs, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        free(imgs[i].rgb);
        free(imgs[i].alpha);
        imgs[i].rgb = imgs[i].alpha = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* DRM / KMS                                                                */
/* ------------------------------------------------------------------------ */



static const char *const blend_names[] = { "Pre-multiplied", "Coverage", "None" };


static const char *const connector_names[] = {
    "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO",
    "LVDS", "Component", "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP",
    "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB",
};

void gem_close(int fd, uint32_t handle)
{
    struct drm_gem_close gc = { .handle = handle };

    if (handle)
        xioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
}

/*
 * Look up a framebuffer. On success the caller owns fb->handle (may be 0
 * when not running as root) and must gem_close() it.
 */
int drm_query_fb(int fd, uint32_t fb_id, struct drm_fb *fb)
{
    struct drm_mode_fb_cmd f1;

    memset(fb, 0, sizeof(*fb));

#ifdef DRM_IOCTL_MODE_GETFB2
    {
        struct drm_mode_fb_cmd2 f2;
        int i;

        memset(&f2, 0, sizeof(f2));
        f2.fb_id = fb_id;
        if (xioctl(fd, DRM_IOCTL_MODE_GETFB2, &f2) == 0) {
            /* Release references to any extra planes; we only use plane 0. */
            for (i = 1; i < 4; i++)
                if (f2.handles[i] && f2.handles[i] != f2.handles[0])
                    gem_close(fd, f2.handles[i]);
            fb->handle = f2.handles[0];
            fb->width = f2.width;
            fb->height = f2.height;
            fb->pitch = f2.pitches[0];
            fb->offset = f2.offsets[0];
            fb->fourcc = f2.pixel_format;
            fb->modifier = (f2.flags & DRM_MODE_FB_MODIFIERS) ?
                           f2.modifier[0] : DRM_FORMAT_MOD_LINEAR;
            fb->fmt = lookup_fourcc(fb->fourcc);
            return 0;
        }
        if (errno == ENOENT)
            return -1;
        /* Kernel older than 5.7: fall back to the legacy ioctl. */
    }
#endif

    memset(&f1, 0, sizeof(f1));
    f1.fb_id = fb_id;
    if (xioctl(fd, DRM_IOCTL_MODE_GETFB, &f1) != 0)
        return -1;
    fb->handle = f1.handle;
    fb->width = f1.width;
    fb->height = f1.height;
    fb->pitch = f1.pitch;
    fb->offset = 0;
    fb->modifier = DRM_FORMAT_MOD_LINEAR;

    if (f1.bpp == 32 && f1.depth == 24)
        fb->fourcc = DRM_FORMAT_XRGB8888;
    else if (f1.bpp == 32 && f1.depth == 32)
        fb->fourcc = DRM_FORMAT_ARGB8888;
    else if (f1.bpp == 32 && f1.depth == 30)
        fb->fourcc = DRM_FORMAT_XRGB2101010;
    else if (f1.bpp == 24)
        fb->fourcc = DRM_FORMAT_RGB888;
    else if (f1.bpp == 16 && f1.depth == 16)
        fb->fourcc = DRM_FORMAT_RGB565;
    else if (f1.bpp == 16 && f1.depth == 15)
        fb->fourcc = DRM_FORMAT_XRGB1555;
    fb->fmt = lookup_fourcc(fb->fourcc);
    return 0;
}

void dmabuf_sync(int dmafd, int end)
{
#ifdef DMA_BUF_IOCTL_SYNC
    struct dma_buf_sync s;

    if (dmafd < 0)
        return;
    s.flags = DMA_BUF_SYNC_READ | (end ? DMA_BUF_SYNC_END : DMA_BUF_SYNC_START);
    xioctl(dmafd, DMA_BUF_IOCTL_SYNC, &s);
#else
    (void)dmafd;
    (void)end;
#endif
}

/*
 * Map framebuffer `fb_id` for reading. Checks that its layout is one we can
 * read (linear, known format). The GEM handle is released once mapped: the
 * mapping keeps the buffer alive. Undo with drm_unmap_fb().
 */
int drm_map_fb(int fd, uint32_t fb_id, struct fbmap *m)
{
    struct drm_prime_handle prime;
    struct drm_fb *fb = &m->fb;
    void *map = MAP_FAILED;
    char fcc[5];

    m->fb_id = fb_id;
    m->map = NULL;
    m->dmafd = -1;
    if (drm_query_fb(fd, fb_id, fb) != 0) {
        err("fb %u: cannot query: %s\n", fb_id, strerror(errno));
        return -1;
    }
    if (!fb->handle) {
        err("fb %u: no buffer handle (are you root?)\n", fb_id);
        return -1;
    }
    if (fb->modifier != DRM_FORMAT_MOD_LINEAR) {
        err("fb %u: tiled/compressed layout (modifier 0x%llx), cannot read it\n",
            fb_id, (unsigned long long)fb->modifier);
        gem_close(fd, fb->handle);
        return -1;
    }
    if (!fb->fmt) {
        err("fb %u: unsupported pixel format %s\n", fb_id,
            fourcc_str(fb->fourcc, fcc));
        gem_close(fd, fb->handle);
        return -1;
    }
    if ((size_t)fb->width * (fb->fmt->bpp / 8) > fb->pitch) {
        err("fb %u: pitch %u too small for width %u\n", fb_id, fb->pitch,
            fb->width);
        gem_close(fd, fb->handle);
        return -1;
    }
    logv("fb %u: %ux%u %s pitch %u\n", fb_id, fb->width, fb->height,
         fourcc_str(fb->fourcc, fcc), fb->pitch);

    m->size = (size_t)fb->offset + (size_t)fb->pitch * fb->height;

    /* Preferred: export as dma-buf and mmap that (works for GPU buffers). */
    memset(&prime, 0, sizeof(prime));
    prime.handle = fb->handle;
    prime.flags = DRM_CLOEXEC;
    if (xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0) {
        m->dmafd = prime.fd;
        map = mmap(NULL, m->size, PROT_READ, MAP_SHARED, m->dmafd, 0);
        if (map == MAP_FAILED)
            logv("fb %u: dma-buf mmap failed: %s\n", fb_id, strerror(errno));
    }

    /* Fallback: treat it as a dumb buffer. */
    if (map == MAP_FAILED) {
        struct drm_mode_map_dumb md;

        if (m->dmafd >= 0) {
            close(m->dmafd);
            m->dmafd = -1;
        }
        memset(&md, 0, sizeof(md));
        md.handle = fb->handle;
        if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0)
            map = mmap(NULL, m->size, PROT_READ, MAP_SHARED, fd, md.offset);
    }

    gem_close(fd, fb->handle);
    fb->handle = 0;
    if (map == MAP_FAILED) {
        err("fb %u: cannot map buffer: %s\n", fb_id, strerror(errno));
        if (m->dmafd >= 0)
            close(m->dmafd);
        m->dmafd = -1;
        return -1;
    }
    m->map = map;
    return 0;
}

void drm_unmap_fb(struct fbmap *m)
{
    if (m->map)
        munmap(m->map, m->size);
    if (m->dmafd >= 0)
        close(m->dmafd);
    m->map = NULL;
    m->dmafd = -1;
    m->fb_id = 0;
}

/*
 * Capture framebuffer `fb_id`. If `crop` is non-NULL only that window
 * ({x, y, w, h}, clipped to the fb) is captured, otherwise the whole fb.
 * With `want_alpha` the alpha channel is kept too (if the format has one).
 */
int drm_capture_fb(int fd, uint32_t fb_id, const uint32_t *crop,
                   struct image *img, int want_alpha)
{
    struct fbmap m;
    const struct drm_fb *fb = &m.fb;
    uint32_t cx = 0, cy = 0, cw, ch;
    size_t start;
    int ret;

    if (drm_map_fb(fd, fb_id, &m) != 0)
        return -1;

    cw = fb->width;
    ch = fb->height;
    if (crop) {
        if (crop[0] >= fb->width || crop[1] >= fb->height) {
            err("fb %u: window %u,%u lies outside %ux%u buffer\n", fb_id,
                crop[0], crop[1], fb->width, fb->height);
            drm_unmap_fb(&m);
            return -1;
        }
        cx = crop[0];
        cy = crop[1];
        cw = crop[2] && crop[2] < fb->width - cx ? crop[2] : fb->width - cx;
        ch = crop[3] && crop[3] < fb->height - cy ? crop[3] : fb->height - cy;
        if (cx || cy || cw != fb->width || ch != fb->height)
            logv("fb %u: cropping %ux%u+%u+%u\n", fb_id, cw, ch, cx, cy);
    }
    start = (size_t)fb->offset + (size_t)cy * fb->pitch +
            (size_t)cx * (fb->fmt->bpp / 8);

    dmabuf_sync(m.dmafd, 0);
    ret = image_from_mapping(m.map + start, fb->pitch, cw, ch, fb->fmt, img,
                             want_alpha);
    dmabuf_sync(m.dmafd, 1);
    drm_unmap_fb(&m);
    return ret;
}

/* Name of the connector driving `crtc_id`, e.g. "DSI-2". */
static void drm_connector_names(int fd, const uint32_t *conns, int nconns,
                                struct screen *s, int n)
{
    int i, j;

    for (i = 0; i < nconns; i++) {
        struct drm_mode_get_connector gc;
        struct drm_mode_get_encoder ge;
        struct drm_mode_modeinfo mode;

        /* count_modes != 0 so the kernel does not force a connector probe. */
        memset(&gc, 0, sizeof(gc));
        gc.connector_id = conns[i];
        gc.count_modes = 1;
        gc.modes_ptr = (uintptr_t)&mode;
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) != 0 || !gc.encoder_id)
            continue;

        memset(&ge, 0, sizeof(ge));
        ge.encoder_id = gc.encoder_id;
        if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) != 0 || !ge.crtc_id)
            continue;

        for (j = 0; j < n; j++) {
            if (s[j].crtc_id != ge.crtc_id || strncmp(s[j].name, "crtc", 4))
                continue;
            if (gc.connector_type < sizeof(connector_names) /
                                    sizeof(connector_names[0]))
                snprintf(s[j].name, sizeof(s[j].name), "%s-%u",
                         connector_names[gc.connector_type],
                         gc.connector_type_id);
            else
                snprintf(s[j].name, sizeof(s[j].name), "conn%u", conns[i]);
        }
    }
}

static int crtc_has_plane(const struct plane *p, int np, uint32_t crtc_id)
{
    int i;

    for (i = 0; i < np; i++)
        if (p[i].crtc_id == crtc_id && p[i].fb_id)
            return 1;
    return 0;
}

/*
 * Fill `s` with all active CRTCs (unsorted): a mode set and something to
 * show, either a primary fb or any plane (`p`, may be empty). Returns count
 * or -1.
 */
int drm_get_screens(int fd, struct screen *s, int max,
                           const struct plane *p, int np)
{
    struct drm_mode_card_res res;
    uint32_t *crtcs, *conns;
    uint32_t ncrtcs, nconns;
    int i, j, n = 0;

    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0)
        return -1;

    ncrtcs = res.count_crtcs;
    nconns = res.count_connectors;
    crtcs = calloc(ncrtcs + 1, sizeof(*crtcs));
    conns = calloc(nconns + 1, sizeof(*conns));
    if (!crtcs || !conns) {
        free(crtcs);
        free(conns);
        return -1;
    }
    memset(&res, 0, sizeof(res));
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.count_crtcs = ncrtcs;
    res.connector_id_ptr = (uintptr_t)conns;
    res.count_connectors = nconns;
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        free(crtcs);
        free(conns);
        return -1;
    }
    /* Counts may have changed in between (hotplug); only trust what we got. */
    if (res.count_crtcs < ncrtcs)
        ncrtcs = res.count_crtcs;
    if (res.count_connectors < nconns)
        nconns = res.count_connectors;

    for (i = 0; i < (int)ncrtcs && n < max; i++) {
        struct drm_mode_crtc c;

        memset(&c, 0, sizeof(c));
        c.crtc_id = crtcs[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) != 0)
            continue;
        logv("CRTC %u: mode_valid=%u fb=%u %ux%u+%u+%u\n", c.crtc_id,
             c.mode_valid, c.fb_id, c.mode.hdisplay, c.mode.vdisplay, c.x, c.y);
        if (!c.mode_valid ||
            (!c.fb_id && !crtc_has_plane(p, np, c.crtc_id)))
            continue;
        memset(&s[n], 0, sizeof(s[n]));
        s[n].crtc_id = c.crtc_id;
        s[n].fb_id = c.fb_id;
        s[n].x = c.x;
        s[n].y = c.y;
        s[n].mode_w = c.mode.hdisplay;
        s[n].mode_h = c.mode.vdisplay;
        snprintf(s[n].name, sizeof(s[n].name), "crtc%u", c.crtc_id);
        n++;
    }
    free(crtcs);

    drm_connector_names(fd, conns, nconns, s, n);
    free(conns);

    /*
     * Each screen normally has its own framebuffer and is captured whole.
     * If one framebuffer is shared by several CRTCs, or a CRTC scans out from
     * an offset, crop that screen's window out of it.
     */
    for (i = 0; i < n; i++) {
        s[i].crop = s[i].x || s[i].y;
        for (j = 0; j < n; j++)
            if (j != i && s[i].fb_id && s[j].fb_id == s[i].fb_id)
                s[i].crop = 1;
    }
    return n;
}

/* Name of enum property `prop_id`'s value `val`; -1 if not found. */
static int enum_name(int fd, uint32_t prop_id, uint64_t val, char name[32])
{
    struct drm_mode_property_enum e[16];
    struct drm_mode_get_property gp;
    uint32_t i;

    memset(&gp, 0, sizeof(gp));
    gp.prop_id = prop_id;
    gp.enum_blob_ptr = (uintptr_t)e;
    gp.count_enum_blobs = 16;
    if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) != 0 ||
        gp.count_enum_blobs > 16)
        return -1;
    for (i = 0; i < gp.count_enum_blobs; i++)
        if (e[i].value == val) {
            memcpy(name, e[i].name, 32);
            name[31] = '\0';
            return 0;
        }
    return -1;
}

/*
 * Read a plane's atomic state: position on screen, source rectangle,
 * stacking order and blending. Enum values are matched by name, since
 * vendor kernels number them differently (Rockchip: Coverage=1, None=2).
 */
void plane_read_props(int fd, struct plane *p)
{
    struct drm_mode_obj_get_properties op;
    uint32_t ids[64];
    uint64_t vals[64];
    uint32_t i, n;
    int have_zpos = 0, geom = 0;

    p->type = -1;
    p->alpha = 0xffff;
    p->blend = BLEND_PREMULTI; /* the DRM default */

    memset(&op, 0, sizeof(op));
    op.obj_id = p->plane_id;
    op.obj_type = DRM_MODE_OBJECT_PLANE;
    op.props_ptr = (uintptr_t)ids;
    op.prop_values_ptr = (uintptr_t)vals;
    op.count_props = 64;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) != 0)
        return;
    n = op.count_props < 64 ? op.count_props : 64;

    for (i = 0; i < n; i++) {
        struct drm_mode_get_property gp;
        uint64_t v = vals[i];
        char en[32];

        memset(&gp, 0, sizeof(gp));
        gp.prop_id = ids[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) != 0)
            continue;
        gp.name[DRM_PROP_NAME_LEN - 1] = '\0';

        if (!strcmp(gp.name, "type"))
            p->type = (int)v;
        else if (!strcmp(gp.name, "CRTC_X"))
            p->crtc_x = (int32_t)(int64_t)v, geom |= 1;
        else if (!strcmp(gp.name, "CRTC_Y"))
            p->crtc_y = (int32_t)(int64_t)v, geom |= 2;
        else if (!strcmp(gp.name, "CRTC_W"))
            p->crtc_w = (uint32_t)v, geom |= 4;
        else if (!strcmp(gp.name, "CRTC_H"))
            p->crtc_h = (uint32_t)v, geom |= 8;
        else if (!strcmp(gp.name, "SRC_X"))
            p->src_x = (uint32_t)v, geom |= 16;
        else if (!strcmp(gp.name, "SRC_Y"))
            p->src_y = (uint32_t)v, geom |= 32;
        else if (!strcmp(gp.name, "SRC_W"))
            p->src_w = (uint32_t)v, geom |= 64;
        else if (!strcmp(gp.name, "SRC_H"))
            p->src_h = (uint32_t)v, geom |= 128;
        else if (!strcmp(gp.name, "zpos"))
            p->zpos = v, have_zpos = 1;
        else if (!strcmp(gp.name, "alpha"))
            p->alpha = (uint32_t)v;
        else if (!strcmp(gp.name, "pixel blend mode") &&
                 enum_name(fd, ids[i], v, en) == 0) {
            if (!strcmp(en, "None"))
                p->blend = BLEND_NONE;
            else if (!strcmp(en, "Coverage"))
                p->blend = BLEND_COVERAGE;
            else
                p->blend = BLEND_PREMULTI;
        }
    }
    p->have_geom = geom == 255 && p->crtc_w && p->crtc_h &&
                   (p->src_w >> 16) && (p->src_h >> 16);
    if (!have_zpos) /* no zpos property: primary, then overlays, cursor on top */
        p->zpos = p->type == 1 ? 0 : p->type == 2 ? 2 : 1;
}

/* All planes and what they show. Returns count or -1. */
int drm_get_planes(int fd, struct plane *p, int max)
{
    struct drm_set_client_cap cap = { DRM_CLIENT_CAP_ATOMIC, 1 };
    struct drm_mode_get_plane_res res;
    uint32_t ids[MAX_PLANES];
    int i, n = 0;

    /* Atomic exposes plane positions and blending; it implies universal
     * planes (primary and cursor listed too), the fallback if refused. */
    if (xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) != 0) {
        cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
        xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    }
    memset(&res, 0, sizeof(res));
    res.plane_id_ptr = (uintptr_t)ids;
    res.count_planes = MAX_PLANES;
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) != 0)
        return -1;
    if (res.count_planes > MAX_PLANES)
        res.count_planes = MAX_PLANES;

    for (i = 0; i < (int)res.count_planes && n < max; i++) {
        struct drm_mode_get_plane gp;

        memset(&gp, 0, sizeof(gp));
        gp.plane_id = ids[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) != 0)
            continue;
        memset(&p[n], 0, sizeof(p[n]));
        p[n].plane_id = gp.plane_id;
        p[n].crtc_id = gp.crtc_id;
        p[n].fb_id = gp.fb_id;
        p[n].type = -1;
        if (gp.crtc_id && gp.fb_id)
            plane_read_props(fd, &p[n]);
        n++;
    }
    return n;
}

/*
 * Find every framebuffer on the device, including ones owned by other
 * processes and ones not currently on screen. GETRESOURCES only lists our
 * own fbs, so probe the (small, densely allocated) object id space instead.
 */
int drm_all_fbs(int fd, uint32_t *ids, int max)
{
    uint32_t id;
    int n = 0;

    for (id = 1; id <= MAX_OBJECT_ID && n < max; id++) {
        struct drm_mode_fb_cmd f;

        memset(&f, 0, sizeof(f));
        f.fb_id = id;
        if (xioctl(fd, DRM_IOCTL_MODE_GETFB, &f) != 0)
            continue;
        gem_close(fd, f.handle);
        ids[n++] = id;
    }
    return n;
}

/*
 * Blend `l`, scaled (nearest neighbour) to w x h, onto `out` at (x0, y0),
 * the way the display controller mixes a plane onto the layers below it.
 */
void blend_layer(struct image *out, const struct image *l, int32_t x0,
                        int32_t y0, uint32_t w, uint32_t h, uint32_t plane_alpha,
                        int blend)
{
    uint32_t pa = plane_alpha >> 8; /* 0..255 */
    uint32_t dx, dy;
    int c;

    for (dy = 0; dy < h; dy++) {
        int64_t y = (int64_t)y0 + dy;
        uint32_t sy = (uint32_t)((uint64_t)dy * l->h / h);

        if (y < 0 || y >= out->h)
            continue;
        for (dx = 0; dx < w; dx++) {
            int64_t x = (int64_t)x0 + dx;
            uint32_t sx = (uint32_t)((uint64_t)dx * l->w / w);
            size_t si = (size_t)sy * l->w + sx;
            const uint8_t *s = l->rgb + si * 3;
            uint8_t *d;
            uint32_t pix_a, a, v;

            if (x < 0 || x >= out->w)
                continue;
            d = out->rgb + ((size_t)y * out->w + x) * 3;
            pix_a = l->alpha && blend != BLEND_NONE ? l->alpha[si] : 255;
            if (pa == 255 && pix_a == 255) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                continue;
            }
            a = (pa * pix_a + 127) / 255;
            for (c = 0; c < 3; c++) {
                if (blend == BLEND_PREMULTI) /* colour already scaled by alpha */
                    v = (s[c] * pa + d[c] * (255 - a) + 127) / 255;
                else
                    v = (s[c] * a + d[c] * (255 - a) + 127) / 255;
                d[c] = v > 255 ? 255 : (uint8_t)v;
            }
        }
    }
}

int cmp_plane_z(const void *a, const void *b)
{
    const struct plane *x = *(const struct plane *const *)a;
    const struct plane *y = *(const struct plane *const *)b;

    if (x->zpos != y->zpos)
        return x->zpos < y->zpos ? -1 : 1;
    if (x->type != y->type) /* primary under overlays under cursor */
        return x->type == 1 ? -1 : y->type == 1 ? 1 : x->type - y->type;
    return x->plane_id < y->plane_id ? -1 : x->plane_id > y->plane_id;
}

/*
 * Build what screen `s` shows from all its planes -- the primary plus any
 * overlays (an on-screen keyboard, a video layer) and the cursor -- stacked
 * by zpos, each at its position, scale and transparency, over black.
 * Returns -1 if there are no usable planes, so the caller can fall back to
 * reading the CRTC's primary framebuffer.
 */
int drm_compose_screen(int fd, const struct screen *s,
                              const struct plane *p, int np, struct image *out)
{
    const struct plane *l[MAX_PLANES];
    int i, n = 0, drawn = 0;

    for (i = 0; i < np; i++)
        if (p[i].crtc_id == s->crtc_id && p[i].fb_id && p[i].have_geom)
            l[n++] = &p[i];
    if (n == 0 || !s->mode_w || !s->mode_h)
        return -1;
    qsort(l, n, sizeof(l[0]), cmp_plane_z);

    out->w = s->mode_w;
    out->h = s->mode_h;
    out->rgb = calloc((size_t)out->w * out->h, 3);
    if (!out->rgb) {
        err("out of memory\n");
        return -1;
    }

    for (i = 0; i < n; i++) {
        uint32_t crop[4] = { l[i]->src_x >> 16, l[i]->src_y >> 16,
                             l[i]->src_w >> 16, l[i]->src_h >> 16 };
        struct image li;

        memset(&li, 0, sizeof(li));
        if (drm_capture_fb(fd, l[i]->fb_id, crop, &li, 1) != 0)
            continue;
        logv("%s plane %u: fb %u %ux%u+%u+%u -> %ux%u%+d%+d zpos %llu "
             "alpha %u%% %s\n", s->name, l[i]->plane_id, l[i]->fb_id,
             crop[2], crop[3], crop[0], crop[1], l[i]->crtc_w, l[i]->crtc_h,
             l[i]->crtc_x, l[i]->crtc_y, (unsigned long long)l[i]->zpos,
             l[i]->alpha * 100 / 0xffff, blend_names[l[i]->blend]);
        blend_layer(out, &li, l[i]->crtc_x, l[i]->crtc_y, l[i]->crtc_w,
                    l[i]->crtc_h, l[i]->alpha, l[i]->blend);
        free(li.rgb);
        free(li.alpha);
        drawn++;
    }
    if (!drawn) {
        free(out->rgb);
        out->rgb = NULL;
        return -1;
    }
    return 0;
}

/* Open the DRM device to use: `dev`, or the first card with an active CRTC. */
int drm_open(const char *dev)
{
    struct screen s[MAX_SCREENS];
    struct plane p[MAX_PLANES];
    char path[32];
    int fd, i, np;

    if (dev) {
        fd = open(dev, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            err("%s: %s\n", dev, strerror(errno));
        return fd;
    }
    for (i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        np = drm_get_planes(fd, p, MAX_PLANES);
        if (drm_get_screens(fd, s, MAX_SCREENS, p, np) > 0) {
            logv("using %s\n", path);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* Print the display layout of the device (-l). */
int drm_list(int fd)
{
    struct screen s[MAX_SCREENS];
    struct plane p[MAX_PLANES];
    uint32_t fbs[256];
    int ns, np, nf, i, j;

    np = drm_get_planes(fd, p, MAX_PLANES);
    ns = drm_get_screens(fd, s, MAX_SCREENS, p, np);
    nf = drm_all_fbs(fd, fbs, 256);

    printf("Screens:\n");
    for (i = 0; i < ns; i++)
        printf("  %-10s crtc %-4u fb %-4u mode %ux%u offset %u,%u%s\n",
               s[i].name, s[i].crtc_id, s[i].fb_id, s[i].mode_w, s[i].mode_h,
               s[i].x, s[i].y, s[i].crop ? "  (window in shared fb)" : "");
    if (ns <= 0)
        printf("  none active\n");

    printf("Planes in use:\n");
    for (i = j = 0; i < np; i++) {
        static const char *const types[] = { "overlay", "primary", "cursor" };

        if (!p[i].crtc_id || !p[i].fb_id) {
            j++;
            continue;
        }
        printf("  plane %-4u crtc %-4u fb %-4u %-7s", p[i].plane_id,
               p[i].crtc_id, p[i].fb_id,
               p[i].type >= 0 && p[i].type <= 2 ? types[p[i].type] : "?");
        if (p[i].have_geom)
            printf(" %ux%u+%u+%u -> %ux%u%+d%+d zpos %llu alpha %u%% %s",
                   p[i].src_w >> 16, p[i].src_h >> 16, p[i].src_x >> 16,
                   p[i].src_y >> 16, p[i].crtc_w, p[i].crtc_h, p[i].crtc_x,
                   p[i].crtc_y, (unsigned long long)p[i].zpos,
                   p[i].alpha * 100 / 0xffff, blend_names[p[i].blend]);
        printf("\n");
    }
    if (np <= 0)
        printf("  none found\n");
    else if (j)
        printf("  (%d unused plane%s)\n", j, j == 1 ? "" : "s");

    printf("Framebuffers:\n");
    for (i = 0; i < nf; i++) {
        struct drm_fb fb;
        char fcc[5];
        const char *use = "not displayed";

        if (drm_query_fb(fd, fbs[i], &fb) != 0)
            continue;
        gem_close(fd, fb.handle);
        for (j = 0; j < np; j++)
            if (p[j].fb_id == fbs[i] && p[j].crtc_id)
                use = "on screen";
        printf("  fb %-4u %ux%u %s pitch %u%s%s  %s\n", fbs[i], fb.width,
               fb.height, fourcc_str(fb.fourcc, fcc), fb.pitch,
               fb.modifier != DRM_FORMAT_MOD_LINEAR ? " tiled" : "",
               fb.fmt ? "" : " (unsupported)", use);
    }
    if (nf <= 0)
        printf("  none found\n");

    if (ns > 1) {
        int shared = 0;

        for (i = 0; i < ns; i++)
            shared |= s[i].crop;
        printf("Layout: %s\n", shared ?
               "screens are windows into a shared framebuffer" :
               "each screen has its own framebuffer");
    }
    return 0;
}

/* Print /dev/fb0 for devices without DRM (-l). Returns -1 if it is unusable. */
int fbdev_list(void)
{
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);

    if (fd < 0 || xioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0 ||
        xioctl(fd, FBIOGET_FSCREENINFO, &fi) != 0 || !vi.xres || !vi.yres) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    printf("Screens:\n");
    printf("  %-10s %ux%u bpp %u (no DRM, so -c does not apply)\n", "fb0",
           vi.xres, vi.yres, vi.bits_per_pixel);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* fbdev capture                                                            */
/* ------------------------------------------------------------------------ */

void fbdev_pixfmt(const struct fb_var_screeninfo *vi, struct pixfmt *fmt)
{
    fmt->bpp = vi->bits_per_pixel;
    fmt->r_off = vi->red.offset;
    fmt->r_len = vi->red.length;
    fmt->g_off = vi->green.offset;
    fmt->g_len = vi->green.length;
    fmt->b_off = vi->blue.offset;
    fmt->b_len = vi->blue.length;
    fmt->a_off = fmt->a_len = 0; /* the screen itself is opaque */
    if (!fmt->r_len && !fmt->g_len && !fmt->b_len) {
        /* Driver did not fill in bitfields; assume the usual layouts. */
        *fmt = *lookup_fourcc(fmt->bpp == 16 ? DRM_FORMAT_RGB565 :
                              fmt->bpp == 24 ? DRM_FORMAT_RGB888 :
                                               DRM_FORMAT_XRGB8888);
    }
}

int fbdev_capture(const char *dev, struct image *img)
{
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    struct pixfmt fmt;
    size_t start, need;
    uint8_t *map;
    int fd, ret;

    fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        logv("%s: %s\n", dev, strerror(errno));
        return -1;
    }
    if (xioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0 ||
        xioctl(fd, FBIOGET_FSCREENINFO, &fi) != 0) {
        err("%s: cannot query screen info: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }
    if (fi.type != FB_TYPE_PACKED_PIXELS ||
        (fi.visual != FB_VISUAL_TRUECOLOR && fi.visual != FB_VISUAL_DIRECTCOLOR) ||
        (vi.bits_per_pixel != 16 && vi.bits_per_pixel != 24 &&
         vi.bits_per_pixel != 32)) {
        err("%s: unsupported layout (type %u visual %u bpp %u)\n", dev,
            fi.type, fi.visual, vi.bits_per_pixel);
        close(fd);
        return -1;
    }

    fbdev_pixfmt(&vi, &fmt);

    start = (size_t)vi.yoffset * fi.line_length +
            (size_t)vi.xoffset * (vi.bits_per_pixel / 8);
    need = start + (size_t)fi.line_length * (vi.yres - 1) +
           (size_t)vi.xres * (vi.bits_per_pixel / 8);
    if (vi.xres == 0 || vi.yres == 0 || need > fi.smem_len) {
        err("%s: bad geometry %ux%u (offset %u,%u)\n", dev, vi.xres, vi.yres,
            vi.xoffset, vi.yoffset);
        close(fd);
        return -1;
    }

    map = mmap(NULL, fi.smem_len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        err("%s: mmap failed: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }
    logv("%s: %ux%u bpp %u stride %u\n", dev, vi.xres, vi.yres,
         vi.bits_per_pixel, fi.line_length);

    ret = image_from_mapping(map + start, fi.line_length, vi.xres, vi.yres,
                             &fmt, img, 0);
    munmap(map, fi.smem_len);
    close(fd);
    return ret;
}

/*
 * The screen EmulationStation is using, on a device with several (RG-DS:
 * 0 bottom/DSI-1, 1 top/DSI-2). Knulli keeps it in /var/run/drmConn as an
 * index into DRM's connector list, disconnected ones included, the way
 * knulli-drminfo numbers them (Miniloong Pocket: 0 HDMI-A-1, 1 DSI-1). Returns that connector's CRTC id and fills `name` (e.g.
 * "DSI-2"), or returns 0 if the file or the connector is not there.
 */
uint32_t drm_active_crtc(int fd, char *name, size_t len)
{
    struct drm_mode_card_res res;
    uint32_t *conns, n, i, crtc = 0;
    FILE *f = fopen("/var/run/drmConn", "r");
    int idx = -1;

    if (f) {
        if (fscanf(f, "%d", &idx) != 1)
            idx = -1;
        fclose(f);
    }
    if (idx < 0)
        return 0;

    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 ||
        !res.count_connectors)
        return 0;
    n = res.count_connectors;
    conns = calloc(n, sizeof(*conns));
    if (!conns)
        return 0;
    memset(&res, 0, sizeof(res));
    res.connector_id_ptr = (uintptr_t)conns;
    res.count_connectors = n;
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        free(conns);
        return 0;
    }
    if (res.count_connectors < n)
        n = res.count_connectors;

    for (i = 0; i < n && !crtc; i++) {
        struct drm_mode_get_connector gc;
        struct drm_mode_get_encoder ge;
        struct drm_mode_modeinfo mode;

        /* count_modes != 0 so the kernel does not force a connector probe. */
        memset(&gc, 0, sizeof(gc));
        gc.connector_id = conns[i];
        gc.count_modes = 1;
        gc.modes_ptr = (uintptr_t)&mode;
        if ((int)i != idx || xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) != 0 ||
            gc.connection != 1 /* DRM_MODE_CONNECTED */)
            continue;
        if (gc.connector_type < sizeof(connector_names) /
                                sizeof(connector_names[0]))
            snprintf(name, len, "%s-%u", connector_names[gc.connector_type],
                     gc.connector_type_id);
        else
            snprintf(name, len, "conn%u", conns[i]);
        memset(&ge, 0, sizeof(ge));
        ge.encoder_id = gc.encoder_id;
        if (gc.encoder_id && xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0)
            crtc = ge.crtc_id;
        break;
    }
    free(conns);
    return crtc;
}

/* ------------------------------------------------------------------------ */
/* Rotation                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * How the panel is mounted, as fbcon numbers it (FB_ROTATE_*): 0 upright,
 * 1 a quarter turn clockwise, 2 upside down, 3 a quarter turn counter-
 * clockwise -- the turn the picture is given to show upright. Knulli sets
 * fbcon=rotate for a panel mounted on its side, as the Miniloong Pocket's
 * 720x960 portrait panel is (3), and apps draw into the framebuffer turned
 * the same way. Same detection as SORP's og_panel_rotation().
 */
int panel_rotation(void)
{
    FILE *f = fopen("/sys/class/graphics/fbcon/rotate", "r");
    int r = 0;

    if (f) {
        if (fscanf(f, "%d", &r) != 1 || r < 0 || r > 3)
            r = 0;
        fclose(f);
    }
    return r;
}

/* Turn an image `turns` quarter turns clockwise, in place. */
int rotate_image(struct image *img, int turns)
{
    uint32_t w = img->w, h = img->h, x, y;
    uint8_t *dst, *d;
    const uint8_t *s;

    turns &= 3;
    if (!turns)
        return 0;
    dst = malloc((size_t)w * h * 3);
    if (!dst) {
        err("out of memory\n");
        return -1;
    }
    for (y = 0; y < h; y++) {
        s = img->rgb + (size_t)y * w * 3;
        for (x = 0; x < w; x++, s += 3) {
            switch (turns) {
            case 1: /* clockwise: (x, y) -> (h-1-y, x), new width h */
                d = dst + ((size_t)x * h + (h - 1 - y)) * 3;
                break;
            case 2: /* half turn: (x, y) -> (w-1-x, h-1-y) */
                d = dst + ((size_t)(h - 1 - y) * w + (w - 1 - x)) * 3;
                break;
            default: /* counter-clockwise: (x, y) -> (y, w-1-x), new width h */
                d = dst + ((size_t)(w - 1 - x) * h + y) * 3;
                break;
            }
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    free(img->rgb);
    img->rgb = dst;
    if (turns != 2) {
        img->w = h;
        img->h = w;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Output                                                                   */
/* ------------------------------------------------------------------------ */

/* Stack images vertically, left-aligned, padding narrower ones with black. */
int stack_images(const struct image *imgs, int n, struct image *out)
{
    uint32_t w = 0, h = 0, y = 0;
    int i;

    for (i = 0; i < n; i++) {
        if (imgs[i].w > w)
            w = imgs[i].w;
        h += imgs[i].h;
    }
    out->w = w;
    out->h = h;
    out->rgb = calloc((size_t)w * h, 3);
    if (!out->rgb) {
        err("out of memory\n");
        return -1;
    }
    for (i = 0; i < n; i++) {
        uint32_t r;

        for (r = 0; r < imgs[i].h; r++, y++)
            memcpy(out->rgb + (size_t)y * w * 3,
                   imgs[i].rgb + (size_t)r * imgs[i].w * 3,
                   (size_t)imgs[i].w * 3);
    }
    return 0;
}


/* mkdir -p for the directory part of `path`. */
void make_parent_dirs(const char *path)
{
    char *p, *tmp = strdup(path);

    if (!tmp)
        return;
    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    free(tmp);
}

/*
 * Parse -c: comma-separated CRTC ids or connector names ("98,DSI-1"). Names
 * are matched against the active screens later, in select_screens().
 */
int parse_crtc_list(const char *arg, struct screen_pick *want, int *nwant)
{
    const char *s = arg;

    while (*s) {
        struct screen_pick *w;
        size_t len = strcspn(s, ",");
        char *end;

        if (*nwant == MAX_SCREENS) {
            err("too many screens (max %d)\n", MAX_SCREENS);
            return -1;
        }
        w = &want[*nwant];
        memset(w, 0, sizeof(*w));
        if (len == 0 || len >= sizeof(w->name)) {
            err("invalid screen list '%s'\n", arg);
            return -1;
        }
        if (*s >= '0' && *s <= '9') {
            unsigned long id;

            errno = 0;
            id = strtoul(s, &end, 10);
            if (errno || id == 0 || id > UINT32_MAX || end != s + len) {
                err("invalid CRTC id in '%s'\n", arg);
                return -1;
            }
            w->crtc_id = (uint32_t)id;
        } else {
            memcpy(w->name, s, len);
        }
        (*nwant)++;
        s += len;
        if (*s)
            s++;
    }
    return 0;
}

/* Choose which screens to capture, in top-to-bottom order. */
int select_screens(struct screen *s, int n, const struct screen_pick *want,
                   int nwant, int reverse)
{
    struct screen sel[MAX_SCREENS];
    int i, j;

    if (nwant > 0) {
        for (i = 0; i < nwant; i++) {
            for (j = 0; j < n; j++)
                if (want[i].crtc_id ? s[j].crtc_id == want[i].crtc_id :
                    strcasecmp(s[j].name, want[i].name) == 0)
                    break;
            if (j == n) {
                if (want[i].crtc_id)
                    err("CRTC %u is not active (see -l)\n", want[i].crtc_id);
                else
                    err("no active screen named %s (see -l)\n", want[i].name);
                return -1;
            }
            sel[i] = s[j];
        }
        memcpy(s, sel, nwant * sizeof(*s));
        n = nwant;
    } else {
        /* Highest id first: on the RG-DS this puts DSI-2 (98) above DSI-1 (74). */
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && s[j].crtc_id > s[j - 1].crtc_id; j--) {
                struct screen t = s[j];
                s[j] = s[j - 1];
                s[j - 1] = t;
            }
    }
    if (reverse)
        for (i = 0; i < n / 2; i++) {
            struct screen t = s[i];
            s[i] = s[n - 1 - i];
            s[n - 1 - i] = t;
        }
    return n;
}
