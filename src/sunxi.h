/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sunxi.h - Allwinner display-engine capture for knulli-record: the
 * display engine writes what the screen shows (all layers composited) into
 * memory as YUV 4:2:0, so the CPU never reads the framebuffer.
 */
#ifndef SUNXI_H
#define SUNXI_H

#include <stddef.h>
#include <stdint.h>

#include "yuv.h"

#define SUNXI_BUFFERS 3

struct sunxi_cap {
    int disp, ion;
    int w, h;                 /* captured size */
    struct {
        int fd, handle;
        uint8_t *map;
        size_t len;
        int64_t ms;           /* when it was queued, -1 if free */
        uint64_t seq;         /* its marker (see sunxi.c) */
    } buf[SUNXI_BUFFERS];
    int next;
    uint64_t seq;
    int held;                 /* buffer handed out by sunxi_step, or -1 */
};

/* The screen's size, or -1 if there is no Allwinner display engine. */
int sunxi_screen_size(int *w, int *h);

/* Start capturing the screen scaled to w x h (even). 0 on success. */
int sunxi_open(struct sunxi_cap *s, int w, int h);

/*
 * Queue a capture for time `ms` and, once the capture queued two calls ago
 * is finished, return it in `out` (planes in the mapped buffer, ready for
 * the CPU) with the time it was queued for. 1: a frame, 0: none yet,
 * 2: the engine did not write that frame (nothing on screen has been
 * committed since; read the framebuffer instead), -1: capture failed.
 * Call sunxi_release() when done with a frame.
 */
int sunxi_step(struct sunxi_cap *s, int64_t ms, struct yuv *out,
               int64_t *out_ms);
void sunxi_release(struct sunxi_cap *s);

void sunxi_close(struct sunxi_cap *s);

#endif
