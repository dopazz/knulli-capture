/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * yuv.c - RGB to YUV 4:2:0 (I420, BT.601 limited range) for knulli-record.
 *
 * The common case -- an upright 32-bit screen at its own size -- converts
 * two rows at a time with NEON. Everything else (rotated panels, scaled or
 * partial planes, other pixel formats, composited screens) goes through a
 * plain C path that maps each output pixel back to its source pixel.
 */
#include <string.h>

#include "yuv.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* BT.601 limited range, 8-bit fixed point. */
static inline uint8_t rgb_y(int r, int g, int b)
{
    return (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}

static inline uint8_t rgb_u(int r, int g, int b)
{
    return (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
}

static inline uint8_t rgb_v(int r, int g, int b)
{
    return (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

void fetch_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
#if defined(__aarch64__)
    while (n >= 128) {
        __asm__ volatile(
            "ldp q0, q1, [%0]\n\t"
            "ldp q2, q3, [%0, #32]\n\t"
            "ldp q4, q5, [%0, #64]\n\t"
            "ldp q6, q7, [%0, #96]\n\t"
            "stp q0, q1, [%1]\n\t"
            "stp q2, q3, [%1, #32]\n\t"
            "stp q4, q5, [%1, #64]\n\t"
            "stp q6, q7, [%1, #96]\n\t"
            :: "r"(src), "r"(dst)
            : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory");
        src += 128;
        dst += 128;
        n -= 128;
    }
#endif
    memcpy(dst, src, n);
}

void yuv_fill_black(struct yuv *f, const struct rect *r)
{
    int y;

    for (y = r->y; y < r->y + r->h; y++)
        memset(f->p[0] + (size_t)y * f->stride[0] + r->x, 16, r->w);
    for (y = r->y / 2; y < (r->y + r->h) / 2; y++) {
        memset(f->p[1] + (size_t)y * f->stride[1] + r->x / 2, 128, r->w / 2);
        memset(f->p[2] + (size_t)y * f->stride[2] + r->x / 2, 128, r->w / 2);
    }
}

/* One 2x2 block: four pixels' Y, and U/V from their average colour. */
static inline void put_block(uint8_t *y0, uint8_t *y1, uint8_t *u, uint8_t *v,
                             const uint8_t *a, const uint8_t *b,
                             const uint8_t *c, const uint8_t *d,
                             int ri, int gi, int bi)
{
    int r = a[ri] + b[ri] + c[ri] + d[ri];
    int g = a[gi] + b[gi] + c[gi] + d[gi];
    int bl = a[bi] + b[bi] + c[bi] + d[bi];

    y0[0] = rgb_y(a[ri], a[gi], a[bi]);
    y0[1] = rgb_y(b[ri], b[gi], b[bi]);
    y1[0] = rgb_y(c[ri], c[gi], c[bi]);
    y1[1] = rgb_y(d[ri], d[gi], d[bi]);
    r = (r + 2) >> 2;
    g = (g + 2) >> 2;
    bl = (bl + 2) >> 2;
    *u = rgb_u(r, g, bl);
    *v = rgb_v(r, g, bl);
}

#if defined(__aarch64__)
static inline uint8x8_t neon_y(uint8x8_t r, uint8x8_t g, uint8x8_t b)
{
    uint16x8_t acc = vmull_u8(r, vdup_n_u8(66));

    acc = vmlal_u8(acc, g, vdup_n_u8(129));
    acc = vmlal_u8(acc, b, vdup_n_u8(25));
    return vadd_u8(vrshrn_n_u16(acc, 8), vdup_n_u8(16));
}

static inline uint8x8_t neon_c(int16x8_t r, int16x8_t g, int16x8_t b,
                               int16_t kr, int16_t kg, int16_t kb)
{
    int16x8_t acc = vmulq_n_s16(r, kr);

    acc = vmlaq_n_s16(acc, g, kg);
    acc = vmlaq_n_s16(acc, b, kb);
    acc = vaddq_s16(vrshrq_n_s16(acc, 8), vdupq_n_s16(128));
    return vqmovun_s16(acc);
}

/* 16 pixels of two rows: R, G, B planes of each row. */
static inline void neon_block16(uint8_t *y0, uint8_t *y1, uint8_t *u,
                                uint8_t *v, uint8x16_t r0, uint8x16_t g0,
                                uint8x16_t b0, uint8x16_t r1, uint8x16_t g1,
                                uint8x16_t b1)
{
    uint16x8_t sr, sg, sb;
    int16x8_t ar, ag, ab;

    vst1q_u8(y0, vcombine_u8(neon_y(vget_low_u8(r0), vget_low_u8(g0),
                                    vget_low_u8(b0)),
                             neon_y(vget_high_u8(r0), vget_high_u8(g0),
                                    vget_high_u8(b0))));
    vst1q_u8(y1, vcombine_u8(neon_y(vget_low_u8(r1), vget_low_u8(g1),
                                    vget_low_u8(b1)),
                             neon_y(vget_high_u8(r1), vget_high_u8(g1),
                                    vget_high_u8(b1))));
    /* Sum each 2x2 block, then round to the average. */
    sr = vpadalq_u8(vpaddlq_u8(r0), r1);
    sg = vpadalq_u8(vpaddlq_u8(g0), g1);
    sb = vpadalq_u8(vpaddlq_u8(b0), b1);
    ar = vreinterpretq_s16_u16(vrshrq_n_u16(sr, 2));
    ag = vreinterpretq_s16_u16(vrshrq_n_u16(sg, 2));
    ab = vreinterpretq_s16_u16(vrshrq_n_u16(sb, 2));
    vst1_u8(u, neon_c(ar, ag, ab, -38, -74, 112));
    vst1_u8(v, neon_c(ar, ag, ab, 112, -94, -18));
}

/* Rows of 4-byte pixels, red/green/blue at constant byte positions. */
#define NEON_ROWS32(name, RI, GI, BI)                                         \
static int name(uint8_t *y0, uint8_t *y1, uint8_t *u, uint8_t *v,            \
                const uint8_t *s0, const uint8_t *s1, int w)                 \
{                                                                             \
    int x;                                                                    \
                                                                              \
    for (x = 0; x + 16 <= w; x += 16) {                                       \
        uint8x16x4_t a = vld4q_u8(s0 + 4 * x);                                \
        uint8x16x4_t b = vld4q_u8(s1 + 4 * x);                                \
                                                                              \
        neon_block16(y0 + x, y1 + x, u + x / 2, v + x / 2,                    \
                     a.val[RI], a.val[GI], a.val[BI],                         \
                     b.val[RI], b.val[GI], b.val[BI]);                        \
    }                                                                         \
    return x;                                                                 \
}

NEON_ROWS32(neon_rows_bgrx, 2, 1, 0) /* XRGB8888 / ARGB8888 */
NEON_ROWS32(neon_rows_rgbx, 0, 1, 2) /* XBGR8888 / ABGR8888 */

static int neon_rows_rgb24(uint8_t *y0, uint8_t *y1, uint8_t *u, uint8_t *v,
                           const uint8_t *s0, const uint8_t *s1, int w)
{
    int x;

    for (x = 0; x + 16 <= w; x += 16) {
        uint8x16x3_t a = vld3q_u8(s0 + 3 * x);
        uint8x16x3_t b = vld3q_u8(s1 + 3 * x);

        neon_block16(y0 + x, y1 + x, u + x / 2, v + x / 2,
                     a.val[0], a.val[1], a.val[2], b.val[0], b.val[1], b.val[2]);
    }
    return x;
}
#endif

void yuv_put_rows(struct yuv *f, int x, int y, const uint8_t *row0,
                  const uint8_t *row1, int w, const struct rgbsrc *fmt)
{
    uint8_t *y0 = f->p[0] + (size_t)y * f->stride[0] + x;
    uint8_t *y1 = y0 + f->stride[0];
    uint8_t *u = f->p[1] + (size_t)(y / 2) * f->stride[1] + x / 2;
    uint8_t *v = f->p[2] + (size_t)(y / 2) * f->stride[2] + x / 2;
    int bpp = fmt->bpp, i = 0;

#if defined(__aarch64__)
    if (bpp == 4 && fmt->ri == 2 && fmt->gi == 1 && fmt->bi == 0)
        i = neon_rows_bgrx(y0, y1, u, v, row0, row1, w);
    else if (bpp == 4 && fmt->ri == 0 && fmt->gi == 1 && fmt->bi == 2)
        i = neon_rows_rgbx(y0, y1, u, v, row0, row1, w);
    else if (bpp == 3 && fmt->ri == 0 && fmt->gi == 1 && fmt->bi == 2)
        i = neon_rows_rgb24(y0, y1, u, v, row0, row1, w);
#endif
    for (; i + 1 < w; i += 2) {
        const uint8_t *a = row0 + (size_t)i * bpp, *c = row1 + (size_t)i * bpp;

        put_block(y0 + i, y1 + i, u + i / 2, v + i / 2, a, a + bpp, c, c + bpp,
                  fmt->ri, fmt->gi, fmt->bi);
    }
}

/*
 * Where output pixel (X, Y) of an rw x rh rectangle comes from in the
 * upright w x h picture it shows, `turns` quarter turns clockwise.
 */
static inline void unturn(int turns, int X, int Y, int rw, int rh, int *ux,
                          int *uy)
{
    switch (turns) {
    case 1: *ux = Y; *uy = rw - 1 - X; break;
    case 2: *ux = rw - 1 - X; *uy = rh - 1 - Y; break;
    case 3: *ux = rh - 1 - Y; *uy = X; break;
    default: *ux = X; *uy = Y; break;
    }
}

void yuv_put_image(struct yuv *f, const struct rect *r, const struct rgbsrc *s,
                   int turns)
{
    int uw = (turns & 1) ? r->h : r->w; /* the rectangle, turned upright */
    int uh = (turns & 1) ? r->w : r->h;
    int half = s->w == 2 * uw && s->h == 2 * uh;
    int bpp = s->bpp, X, Y, k;

    if (uw <= 0 || uh <= 0 || s->w <= 0 || s->h <= 0)
        return;
    if (!turns && s->w == uw && s->h == uh) {
        for (Y = 0; Y + 1 < r->h; Y += 2)
            yuv_put_rows(f, r->x, r->y + Y, s->data + (size_t)Y * s->pitch,
                         s->data + (size_t)(Y + 1) * s->pitch, r->w, s);
        return;
    }

    for (Y = 0; Y + 1 < r->h; Y += 2) {
        uint8_t *y0 = f->p[0] + (size_t)(r->y + Y) * f->stride[0] + r->x;
        uint8_t *y1 = y0 + f->stride[0];
        uint8_t *u = f->p[1] + (size_t)((r->y + Y) / 2) * f->stride[1] + r->x / 2;
        uint8_t *v = f->p[2] + (size_t)((r->y + Y) / 2) * f->stride[2] + r->x / 2;

        for (X = 0; X + 1 < r->w; X += 2) {
            const uint8_t *px[4];
            uint8_t avg[4][3];

            for (k = 0; k < 4; k++) {
                int ux, uy, sx, sy;

                unturn(turns, X + (k & 1), Y + (k >> 1), r->w, r->h, &ux, &uy);
                if (half) {
                    /* Exact half size: average the 2x2 source block. */
                    const uint8_t *p = s->data + (size_t)(2 * uy) * s->pitch +
                                       (size_t)(2 * ux) * bpp;
                    const uint8_t *q = p + s->pitch;

                    avg[k][0] = (p[s->ri] + p[bpp + s->ri] + q[s->ri] +
                                 q[bpp + s->ri] + 2) >> 2;
                    avg[k][1] = (p[s->gi] + p[bpp + s->gi] + q[s->gi] +
                                 q[bpp + s->gi] + 2) >> 2;
                    avg[k][2] = (p[s->bi] + p[bpp + s->bi] + q[s->bi] +
                                 q[bpp + s->bi] + 2) >> 2;
                    px[k] = avg[k];
                    continue;
                }
                sx = (int)((int64_t)ux * s->w / uw);
                sy = (int)((int64_t)uy * s->h / uh);
                px[k] = s->data + (size_t)sy * s->pitch + (size_t)sx * bpp;
            }
            if (half)
                put_block(y0 + X, y1 + X, u + X / 2, v + X / 2, px[0], px[1],
                          px[2], px[3], 0, 1, 2);
            else
                put_block(y0 + X, y1 + X, u + X / 2, v + X / 2, px[0], px[1],
                          px[2], px[3], s->ri, s->gi, s->bi);
        }
    }
}

/* Turn a w x h plane `turns` quarter turns clockwise, in 16x16 tiles. */
static void rotate_plane(uint8_t *d, int dstride, const uint8_t *s,
                         int sstride, int w, int h, int turns)
{
    int tx, ty, x, y;

    for (ty = 0; ty < h; ty += 16) {
        int ye = ty + 16 < h ? ty + 16 : h;

        for (tx = 0; tx < w; tx += 16) {
            int xe = tx + 16 < w ? tx + 16 : w;

            for (y = ty; y < ye; y++) {
                const uint8_t *sp = s + (size_t)y * sstride;

                for (x = tx; x < xe; x++) {
                    switch (turns) {
                    case 1: /* (x, y) -> (h-1-y, x) */
                        d[(size_t)x * dstride + (h - 1 - y)] = sp[x];
                        break;
                    case 2: /* (x, y) -> (w-1-x, h-1-y) */
                        d[(size_t)(h - 1 - y) * dstride + (w - 1 - x)] = sp[x];
                        break;
                    default: /* (x, y) -> (y, w-1-x) */
                        d[(size_t)(w - 1 - x) * dstride + y] = sp[x];
                        break;
                    }
                }
            }
        }
    }
}

void yuv_rotate(struct yuv *f, const struct rect *r, const struct yuv *s,
                int turns)
{
    int i;

    for (i = 0; i < 3; i++) {
        int sh = i ? 1 : 0; /* chroma planes are half size */

        rotate_plane(f->p[i] + (size_t)(r->y >> sh) * f->stride[i] + (r->x >> sh),
                     f->stride[i], s->p[i], s->stride[i], s->w >> sh,
                     s->h >> sh, turns);
    }
}

void yuv_copy(struct yuv *f, const struct rect *r, const struct yuv *s)
{
    int i, y;

    for (i = 0; i < 3; i++) {
        int sh = i ? 1 : 0;
        int w = (s->w < r->w ? s->w : r->w) >> sh;
        int h = (s->h < r->h ? s->h : r->h) >> sh;

        for (y = 0; y < h; y++)
            memcpy(f->p[i] + (size_t)((r->y >> sh) + y) * f->stride[i] +
                   (r->x >> sh),
                   s->p[i] + (size_t)y * s->stride[i], (size_t)w);
    }
}
