/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * yuv.h - RGB to YUV 4:2:0 (I420, BT.601 limited range) for knulli-record.
 */
#ifndef YUV_H
#define YUV_H

#include <stddef.h>
#include <stdint.h>

/* An I420 picture: full-size Y plane, half-size U and V planes. */
struct yuv {
    uint8_t *p[3];
    int stride[3];
    int w, h;
};

/*
 * A packed RGB image in cached memory: `bpp` bytes per pixel (3 or 4) with
 * red, green and blue at byte `ri`, `gi`, `bi` of each pixel.
 */
struct rgbsrc {
    const uint8_t *data;
    size_t pitch;
    int w, h;
    int bpp, ri, gi, bi;
};

/* A rectangle in the output picture (even x, y, w, h). */
struct rect {
    int x, y, w, h;
};

/*
 * Copy `n` bytes out of a scanout buffer. Those are mapped uncached on the
 * handhelds, where plain loads crawl (26 MB/s for 32-bit loads, 125 MB/s
 * for memcpy on the RK3566); 128-bit pair loads reach ~200 MB/s.
 */
void fetch_bytes(uint8_t *dst, const uint8_t *src, size_t n);

/* Paint a rectangle black. */
void yuv_fill_black(struct yuv *f, const struct rect *r);

/*
 * Convert two RGB rows of `w` pixels (w even) into rows y, y+1 of the
 * picture starting at column x (x, y even), unscaled and unrotated.
 */
void yuv_put_rows(struct yuv *f, int x, int y, const uint8_t *row0,
                  const uint8_t *row1, int w, const struct rgbsrc *fmt);

/*
 * Copy picture `s` into rectangle `r` of `f` (r: s's size, turned), turned
 * `turns` (1-3) quarter turns clockwise. s->w and s->h must be even.
 */
void yuv_rotate(struct yuv *f, const struct rect *r, const struct yuv *s,
                int turns);

/* Copy picture `s` into rectangle `r` of `f`, unturned (s->w, s->h even). */
void yuv_copy(struct yuv *f, const struct rect *r, const struct yuv *s);

/*
 * Draw `s` into rectangle `r` of the picture, turned `turns` quarter turns
 * clockwise and scaled to fit (nearest neighbour; a 2x2 average for an exact
 * half size).
 */
void yuv_put_image(struct yuv *f, const struct rect *r, const struct rgbsrc *s,
                   int turns);

#endif
