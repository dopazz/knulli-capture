/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * kms.h - display capture shared by knulli-screenshot and knulli-record:
 * DRM/KMS screens, planes and framebuffers, the fbdev fallback, pixel
 * formats and panel rotation.
 */
#ifndef KMS_H
#define KMS_H

#include <stddef.h>
#include <stdint.h>

/* Shown by -h. Set by the Makefile (UTC); the compiler's date otherwise. */
#ifndef BUILD_TIMESTAMP
#define BUILD_TIMESTAMP __DATE__ " " __TIME__
#endif

#define MAX_SCREENS   8
#define MAX_PLANES    64
#define MAX_OBJECT_ID 4096 /* highest DRM object id probed for framebuffers */

extern const char *prog_name; /* prefix for err() messages */
extern int verbose;
extern int quiet;             /* suppress err() while probing optional buffers */

void logv(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int xioctl(int fd, unsigned long req, void *arg);

/* ------------------------------------------------------------------------ */
/* Pixel formats and images                                                 */

/* Little-endian packed pixel described by bit offsets/lengths. */
struct pixfmt {
    int bpp;
    int r_off, r_len;
    int g_off, g_len;
    int b_off, b_len;
    int a_off, a_len; /* a_len 0: no alpha (X formats), pixel is opaque */
};

/* Packed RGB24 in memory order R, G, B, as a struct pixfmt. */
extern const struct pixfmt pixfmt_rgb24;

struct image {
    uint32_t w, h;
    uint8_t *rgb;   /* w * h * 3 bytes */
    uint8_t *alpha; /* w * h bytes; only for plane layers that have alpha */
    char label[40];
};

const struct pixfmt *lookup_fourcc(uint32_t fourcc);
const char *fourcc_str(uint32_t fourcc, char buf[5]);

static inline uint8_t extract(uint32_t v, int off, int len)
{
    uint32_t mask, c;

    if (len <= 0)
        return 0;
    mask = (len >= 32) ? 0xffffffffu : ((1u << len) - 1);
    c = (v >> off) & mask;
    if (len >= 8)
        return (uint8_t)(c >> (len - 8));
    return (uint8_t)((c * 255 + mask / 2) / mask);
}

void free_images(struct image *imgs, int n);
int rotate_image(struct image *img, int turns);
int stack_images(const struct image *imgs, int n, struct image *out);

/* ------------------------------------------------------------------------ */
/* DRM / KMS                                                                */

struct drm_fb {
    uint32_t width, height, pitch, offset, handle, fourcc;
    uint64_t modifier;
    const struct pixfmt *fmt; /* NULL if unsupported */
};

/* A framebuffer mapped for reading (see drm_map_fb). */
struct fbmap {
    uint32_t fb_id;
    struct drm_fb fb;
    uint8_t *map;     /* the whole buffer; pixels start at map + fb.offset */
    size_t size;
    int dmafd;        /* dma-buf fd, -1 for a dumb-buffer mapping */
};

/* One active display: a CRTC and the region of its framebuffer it shows. */
struct screen {
    uint32_t crtc_id, fb_id;
    uint32_t x, y;           /* CRTC scanout offset within the fb */
    uint32_t mode_w, mode_h; /* display resolution */
    int crop;                /* capture only the x,y,mode_w,mode_h window */
    char name[32];           /* connector name, e.g. "DSI-1" */
};

/* A screen asked for with -c: a CRTC id, or a connector name when 0. */
struct screen_pick {
    uint32_t crtc_id;
    char name[32];
};

/* How a plane's pixels combine with what is below (DRM "pixel blend mode"). */
enum { BLEND_PREMULTI, BLEND_COVERAGE, BLEND_NONE };

/* A hardware plane: one layer of what a CRTC shows. */
struct plane {
    uint32_t plane_id, crtc_id, fb_id;
    int type;                    /* 0 overlay, 1 primary, 2 cursor, -1 unknown */
    int have_geom;               /* CRTC_* and SRC_* properties were read */
    int32_t crtc_x, crtc_y;      /* where it lands on the screen... */
    uint32_t crtc_w, crtc_h;     /* ...and how big */
    uint32_t src_x, src_y;       /* the part of the fb it shows, 16.16 */
    uint32_t src_w, src_h;
    uint64_t zpos;               /* stacking order, higher on top */
    uint32_t alpha;              /* plane opacity, 0..0xffff */
    int blend;                   /* BLEND_* */
};

void gem_close(int fd, uint32_t handle);
int drm_query_fb(int fd, uint32_t fb_id, struct drm_fb *fb);
int drm_map_fb(int fd, uint32_t fb_id, struct fbmap *m);
void drm_unmap_fb(struct fbmap *m);
void dmabuf_sync(int dmafd, int end);
int drm_capture_fb(int fd, uint32_t fb_id, const uint32_t *crop,
                   struct image *img, int want_alpha);
int drm_get_planes(int fd, struct plane *p, int max);
void plane_read_props(int fd, struct plane *p);
int drm_get_screens(int fd, struct screen *s, int max,
                    const struct plane *p, int np);
int drm_all_fbs(int fd, uint32_t *ids, int max);
void blend_layer(struct image *out, const struct image *l, int32_t x0,
                 int32_t y0, uint32_t w, uint32_t h, uint32_t plane_alpha,
                 int blend);
int cmp_plane_z(const void *a, const void *b);
int drm_compose_screen(int fd, const struct screen *s,
                       const struct plane *p, int np, struct image *out);
int drm_open(const char *dev);
int drm_list(int fd);
int fbdev_list(void);
int parse_crtc_list(const char *arg, struct screen_pick *want, int *nwant);
int select_screens(struct screen *s, int n, const struct screen_pick *want,
                   int nwant, int reverse);
uint32_t drm_active_crtc(int fd, char *name, size_t len);

/* ------------------------------------------------------------------------ */
/* fbdev, rotation, files                                                   */

int fbdev_capture(const char *dev, struct image *img);
/* Describe an fbdev's pixel layout; fills fmt from the var screeninfo. */
struct fb_var_screeninfo;
void fbdev_pixfmt(const struct fb_var_screeninfo *vi, struct pixfmt *fmt);
int panel_rotation(void);
void make_parent_dirs(const char *path);

#endif
