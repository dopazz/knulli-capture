/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rga.h - Rockchip RGA (2D engine) for knulli-record: reads a scanout
 * buffer and writes it as I420 (turned and scaled as needed) with no CPU
 * work. librga is loaded at run time; without it the CPU path is used.
 */
#ifndef RGA_H
#define RGA_H

#include <stdint.h>

#include "yuv.h"

/* Load librga; 0 if the RGA can be used. */
int rga_open(void);

/* RGA pixel format for a DRM fourcc, or -1 if the RGA cannot read it. */
int rga_format(uint32_t fourcc);

/* Register a buffer with the RGA; 0 on failure. Undo with rga_release(). */
uint32_t rga_import_fd(int fd, int width, int height, int format);
uint32_t rga_import_mem(void *mem, int width, int height, int format);
void rga_release(uint32_t handle);

/* The same, keeping the alpha channel (for blending), if it has one. */
int rga_format_alpha(uint32_t fourcc);

/* Formats for rga_import_mem(): an I420 picture, a BGRX screen. */
int rga_i420(void);
int rga_bgrx(void);

/* How rga_draw() puts the source on the destination. */
enum { RGA_COPY, RGA_OVER_PREMULT, RGA_OVER_COVERAGE };

/*
 * Draw rectangle `sr` of source `src` (w x h, rows `stride` pixels apart,
 * RGA format `fmt`) into rectangle `dr` of `dst` (dw x dh, format `dfmt`),
 * turned `turns` quarter turns clockwise and scaled to fit; copied, or
 * blended over what is there with plane opacity `alpha` (0-255). Returns 0,
 * or -1 with the reason in `why`.
 */
int rga_draw(uint32_t src, int w, int h, int stride, int fmt,
             const struct rect *sr, int alpha, uint32_t dst, int dw, int dh,
             int dfmt, const struct rect *dr, int turns, int mode,
             const char **why);

/*
 * Convert rectangle `sr` of source `src` (w x h, rows `stride` pixels
 * apart, RGA format `fmt`) into rectangle `dr` of the I420 picture `dst`
 * (dw x dh), turned `turns` quarter turns clockwise and scaled to fit.
 * Returns 0, or -1 with the reason in `why`.
 */
int rga_blit(uint32_t src, int w, int h, int stride, int fmt,
             const struct rect *sr, uint32_t dst, int dw, int dh,
             const struct rect *dr, int turns, const char **why);

#endif
