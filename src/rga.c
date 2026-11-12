/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rga.c - Rockchip RGA (2D engine) for knulli-record.
 *
 * Reading scanout memory with the CPU costs ~6 ms per 640x480 screen on
 * the RK3566 (it is uncached), plus the conversion to YUV and any turning.
 * The RGA does all of it from the buffer's dma-buf in ~2 ms of its own
 * time and ~0.25 ms of CPU: measured 720x960 XRGB -> I420 turned a quarter
 * turn on the Miniloong Pocket.
 *
 * librga (Rockchip's userspace library, librga.so.2) is opened with dlopen
 * so the recorder has no build or run-time dependency on it. The few
 * definitions needed are copied from its im2d headers (API v1.10.4, as
 * shipped by Knulli); rga_buffer_t is passed by value, so its layout must
 * match the library's.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>

#include "kms.h"
#include "rga.h"

/* ---- from librga's im2d_type.h / rga.h (v1.10.4) ----------------------- */

typedef uint32_t rga_buffer_handle_t;

typedef struct {
    int x, y, width, height;
} im_rect;

typedef struct {
    int max, min;
} im_colorkey_range;

typedef struct im_nn {
    int scale_r, scale_g, scale_b;
    int offset_r, offset_g, offset_b;
} im_nn_t;

typedef struct {
    void *vir_addr;
    void *phy_addr;
    int fd;
    int width, height, wstride, hstride, format;
    int color_space_mode;
    union {
        int global_alpha;
        struct {
            uint16_t alpha0, alpha1;
        } alpha_bit;
    } u;
    int rd_mode;
    int color;
    im_colorkey_range colorkey_range;
    im_nn_t nn;
    int rop_code;
    rga_buffer_handle_t handle;
} rga_buffer_t;

typedef struct im_handle_param {
    uint32_t width, height, format;
} im_handle_param_t;

#define IM_STATUS_SUCCESS          1
#define IM_STATUS_NOERROR          2
#define IM_HAL_TRANSFORM_ROT_90    (1 << 0)
#define IM_HAL_TRANSFORM_ROT_180   (1 << 1)
#define IM_HAL_TRANSFORM_ROT_270   (1 << 2)
#define IM_ALPHA_BLEND_SRC_OVER    (1 << 6)
#define IM_SYNC                    (1 << 19)
#define IM_ALPHA_BLEND_PRE_MUL     (1 << 25) /* multiply src by its alpha */
#define IM_RGB_TO_YUV_BT601_LIMIT  (2 << 2)

#define RK_FORMAT_RGBA_8888        (0x0 << 8)
#define RK_FORMAT_RGBX_8888        (0x1 << 8)
#define RK_FORMAT_BGRA_8888        (0x3 << 8)
#define RK_FORMAT_YCbCr_420_P      (0xb << 8)
#define RK_FORMAT_BGRX_8888        (0x16 << 8)

/* ------------------------------------------------------------------------ */

static struct {
    void *lib;
    rga_buffer_handle_t (*import_fd)(int, im_handle_param_t *);
    rga_buffer_handle_t (*import_va)(void *, im_handle_param_t *);
    int (*release)(rga_buffer_handle_t);
    rga_buffer_t (*wrap)(rga_buffer_handle_t, int, int, int, int, int);
    int (*process)(rga_buffer_t, rga_buffer_t, rga_buffer_t, im_rect, im_rect,
                   im_rect, int);
    const char *(*strerr)(int);
} lib;

int rga_open(void)
{
    if (lib.lib)
        return 0;
    if (access("/dev/rga", R_OK | W_OK) != 0)
        return -1;
    lib.lib = dlopen("librga.so.2", RTLD_NOW);
    if (!lib.lib) {
        logv("rga: %s\n", dlerror());
        return -1;
    }
    *(void **)&lib.import_fd = dlsym(lib.lib, "importbuffer_fd");
    *(void **)&lib.import_va = dlsym(lib.lib, "importbuffer_virtualaddr");
    *(void **)&lib.release = dlsym(lib.lib, "releasebuffer_handle");
    *(void **)&lib.wrap = dlsym(lib.lib, "wrapbuffer_handle_t");
    *(void **)&lib.process = dlsym(lib.lib, "improcess");
    *(void **)&lib.strerr = dlsym(lib.lib, "imStrError_t");
    if (!lib.import_fd || !lib.import_va || !lib.release || !lib.wrap ||
        !lib.process || !lib.strerr) {
        logv("rga: librga lacks the im2d API\n");
        dlclose(lib.lib);
        lib.lib = NULL;
        return -1;
    }
    return 0;
}

int rga_format(uint32_t fourcc)
{
    switch (fourcc) { /* DRM names the bits high to low, the RGA bytes in order */
    case DRM_FORMAT_XRGB8888: return RK_FORMAT_BGRX_8888;
    case DRM_FORMAT_ARGB8888: return RK_FORMAT_BGRX_8888; /* opaque on screen */
    case DRM_FORMAT_XBGR8888: return RK_FORMAT_RGBX_8888;
    case DRM_FORMAT_ABGR8888: return RK_FORMAT_RGBX_8888;
    default: return -1;
    }
}

int rga_format_alpha(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888: return RK_FORMAT_BGRA_8888;
    case DRM_FORMAT_ABGR8888: return RK_FORMAT_RGBA_8888;
    default: return rga_format(fourcc); /* no alpha: opaque pixels */
    }
}

int rga_i420(void)
{
    return RK_FORMAT_YCbCr_420_P;
}

int rga_bgrx(void)
{
    return RK_FORMAT_BGRX_8888;
}

uint32_t rga_import_fd(int fd, int width, int height, int format)
{
    im_handle_param_t p = { (uint32_t)width, (uint32_t)height, (uint32_t)format };

    return lib.lib ? lib.import_fd(fd, &p) : 0;
}

uint32_t rga_import_mem(void *mem, int width, int height, int format)
{
    im_handle_param_t p = { (uint32_t)width, (uint32_t)height, (uint32_t)format };

    return lib.lib ? lib.import_va(mem, &p) : 0;
}

void rga_release(uint32_t handle)
{
    if (lib.lib && handle)
        lib.release(handle);
}

int rga_draw(uint32_t src, int w, int h, int stride, int fmt,
             const struct rect *sr, int alpha, uint32_t dst, int dw, int dh,
             int dfmt, const struct rect *dr, int turns, int mode,
             const char **why)
{
    static const int rot[4] = { 0, IM_HAL_TRANSFORM_ROT_90,
                                IM_HAL_TRANSFORM_ROT_180,
                                IM_HAL_TRANSFORM_ROT_270 };
    rga_buffer_t s, d, pat;
    im_rect srect = { sr->x, sr->y, sr->w, sr->h };
    im_rect drect = { dr->x, dr->y, dr->w, dr->h };
    im_rect none;
    int st, usage = IM_SYNC | rot[turns & 3];

    s = lib.wrap(src, w, h, stride, h, fmt);
    d = lib.wrap(dst, dw, dh, dw, dh, dfmt);
    if (dfmt == RK_FORMAT_YCbCr_420_P)
        d.color_space_mode = IM_RGB_TO_YUV_BT601_LIMIT; /* as yuv.c converts */
    /* Blending as the display controller does it (checked against the
     * CPU compositor on the RG-DS keyboard: mean difference 0.4 of 255).
     * Note librga's naming: PRE_MUL means "multiply the source by its
     * alpha", i.e. a source that is *not* premultiplied (DRM "Coverage"). */
    if (mode != RGA_COPY) {
        s.u.global_alpha = alpha;
        usage |= IM_ALPHA_BLEND_SRC_OVER;
        if (mode == RGA_OVER_COVERAGE)
            usage |= IM_ALPHA_BLEND_PRE_MUL;
    }
    memset(&pat, 0, sizeof(pat));
    memset(&none, 0, sizeof(none));
    st = lib.process(s, d, pat, srect, drect, none, usage);
    if (st == IM_STATUS_SUCCESS || st == IM_STATUS_NOERROR)
        return 0;
    *why = lib.strerr(st);
    return -1;
}

int rga_blit(uint32_t src, int w, int h, int stride, int fmt,
             const struct rect *sr, uint32_t dst, int dw, int dh,
             const struct rect *dr, int turns, const char **why)
{
    return rga_draw(src, w, h, stride, fmt, sr, 255, dst, dw, dh,
                    RK_FORMAT_YCbCr_420_P, dr, turns, RGA_COPY, why);
}
