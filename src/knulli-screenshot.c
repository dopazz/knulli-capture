/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * knulli-screenshot - capture the display(s) to PNG.
 *
 * C replacement for the knulli-screenshot shell script. Instead of shelling
 * out to fbgrab/ffmpeg it reads the scanout buffers directly:
 *
 *   1. DRM/KMS: each active CRTC (screen) is rebuilt from all its planes --
 *      primary, overlays such as the RG-DS on-screen keyboard, cursor --
 *      stacked by zpos and blended at their position, scale and alpha.
 *      Framebuffers are mapped as dma-buf, or dumb buffers as a fallback.
 *      Without atomic plane properties the CRTC's primary framebuffer is
 *      read alone (cropped out when several CRTCs share one).
 *      Screens are stacked into one PNG (highest CRTC id on top, which puts
 *      DSI-2 above DSI-1 on the RG-DS) or, with -s, saved one PNG each.
 *   2. fbdev: if DRM capture is not possible, /dev/fb0 is read instead.
 *
 * With -a, every other framebuffer that exists on the device (stale buffers
 * left over from app / video mode switches, the fbdev console buffer, ...)
 * is dumped to its own PNG as well.
 *
 * A panel mounted on its side (fbcon=rotate, as on the Miniloong Pocket) is
 * undone so the PNG comes out upright; -R overrides the detected rotation.
 *
 * Files are named knulli-screenshot_<wwww>x<h>_<timestamp>[_<label>].png,
 * the width zero-padded to 4 digits.
 *
 * Must run as root (DRM only hands out framebuffer handles to CAP_SYS_ADMIN).
 * The capture code is shared with knulli-record (kms.c). Only depends on zlib.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "kms.h"

#define DEFAULT_DIR    "/userdata/screenshots"
#define DEFAULT_PREFIX "knulli-screenshot"
#define MAX_EXTRA_FBS  32

/* ------------------------------------------------------------------------ */
/* Output                                                                   */
/* ------------------------------------------------------------------------ */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static int png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8], crc[4];
    uLong c;

    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    c = crc32(0L, (const Bytef *)type, 4);
    if (len)
        c = crc32(c, data, len);
    put_be32(crc, (uint32_t)c);

    return fwrite(hdr, 1, 8, f) == 8 &&
           (len == 0 || fwrite(data, 1, len, f) == len) &&
           fwrite(crc, 1, 4, f) == 4 ? 0 : -1;
}

static int write_png_file(const char *path, const struct image *img)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    size_t stride = (size_t)img->w * 3 + 1;
    size_t rawlen = stride * img->h;
    uint8_t ihdr[13];
    uint8_t *raw, *z;
    uLongf zlen;
    uint32_t y;
    FILE *f;
    int ok;

    raw = malloc(rawlen);
    zlen = compressBound(rawlen);
    z = malloc(zlen);
    if (!raw || !z) {
        free(raw);
        free(z);
        err("out of memory\n");
        return -1;
    }
    for (y = 0; y < img->h; y++) {
        raw[y * stride] = 0; /* filter: none */
        memcpy(raw + y * stride + 1, img->rgb + (size_t)y * img->w * 3,
               stride - 1);
    }
    if (compress2(z, &zlen, raw, rawlen, Z_DEFAULT_COMPRESSION) != Z_OK) {
        free(raw);
        free(z);
        err("zlib compression failed\n");
        return -1;
    }
    free(raw);

    put_be32(ihdr, img->w);
    put_be32(ihdr + 4, img->h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: RGB */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */

    f = fopen(path, "wb");
    if (!f) {
        err("cannot create %s: %s\n", path, strerror(errno));
        free(z);
        return -1;
    }
    ok = fwrite(sig, 1, 8, f) == 8 &&
         png_chunk(f, "IHDR", ihdr, sizeof(ihdr)) == 0 &&
         png_chunk(f, "IDAT", z, (uint32_t)zlen) == 0 &&
         png_chunk(f, "IEND", NULL, 0) == 0 &&
         fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0)
        ok = 0;
    free(z);
    if (!ok) {
        err("error writing %s: %s\n", path, strerror(errno));
        unlink(path);
        return -1;
    }
    return 0;
}

/* Write to a temp file and rename, so a partial PNG never appears. */
static int write_png(const char *path, const struct image *img)
{
    char *tmppath;
    int ret;

    if (asprintf(&tmppath, "%s.tmp", path) < 0) {
        err("out of memory\n");
        return -1;
    }
    ret = write_png_file(tmppath, img);
    if (ret == 0 && rename(tmppath, path) != 0) {
        err("cannot rename %s to %s: %s\n", tmppath, path, strerror(errno));
        unlink(tmppath);
        ret = -1;
    }
    free(tmppath);
    if (ret == 0) {
        logv("saved %s\n", path);
        printf("%s\n", path);
    }
    return ret;
}

/* Output file name parts: <prefix>_<w>x<h>[_<stamp>][_<label>].png */
struct outname {
    char prefix[PATH_MAX];
    char stamp[32]; /* empty when the user gave a file name */
};

static int write_named(const struct outname *on, const char *label,
                       const struct image *img)
{
    char path[PATH_MAX + 64];
    int len;

    len = snprintf(path, sizeof(path), "%s_%04ux%u%s%s%s%s.png", on->prefix,
                   img->w, img->h, *on->stamp ? "_" : "", on->stamp,
                   label && *label ? "_" : "", label ? label : "");
    if (len < 0 || (size_t)len >= sizeof(path)) {
        err("path too long\n");
        return -1;
    }
    return write_png(path, img);
}

/* ------------------------------------------------------------------------ */

static void usage(FILE *f)
{
    fprintf(f,
        "Usage: knulli-screenshot [options] [file.png]\n"
        "Build: " BUILD_TIMESTAMP "\n"
        "\n"
        "Captures all active displays to PNG. Several active displays are\n"
        "stacked vertically into one image, highest CRTC id on top.\n"
        "\n"
        "Files are named\n"
        "  " DEFAULT_DIR "/" DEFAULT_PREFIX "_<wwww>x<h>_YYYYmmdd_HHhMMmSSs.png\n"
        "with the width zero-padded to 4 digits. If file.png is given, files\n"
        "are named file_<wwww>x<h>.png instead.\n"
        "\n"
        "  -s          save each screen to its own file, named ..._<screen>.png\n"
        "  -a          also dump every framebuffer not on screen (stale buffers\n"
        "              from app/video mode switches), named ..._fb<id>.png\n"
        "  -l          list screens, planes and framebuffers, then exit\n"
        "  -d DEVICE   DRM device (default: first /dev/dri/cardN with a display)\n"
        "  -c SCREENS  capture only these screens, top to bottom: CRTC ids or\n"
        "              connector names from -l, comma-separated (RG-DS:\n"
        "              -c DSI-2 top screen, -c DSI-1 bottom screen)\n"
        "  -r          reverse the stacking order\n"
        "  -R N        panel rotation 0-3, fbcon numbering (default: read from\n"
        "              /sys/class/graphics/fbcon/rotate); the capture is turned\n"
        "              back upright. -R 0 saves the raw framebuffer orientation\n"
        "  -v          verbose output on stderr\n"
        "  -h          show this help\n"
        "\n"
        "The path of every file written is printed on stdout.\n");
}

/*
 * Dump every framebuffer that is not on screen (`p`: the planes as they were
 * when the screens were captured, so those layers are already in the shot).
 */
static void dump_extra_fbs(int fd, const struct screen *s, int ns,
                           const struct plane *p, int np,
                           const struct outname *on, int turns)
{
    uint32_t fbs[MAX_EXTRA_FBS + MAX_SCREENS];
    int nf, i, j;

    nf = drm_all_fbs(fd, fbs, MAX_EXTRA_FBS + MAX_SCREENS);
    logv("found %d framebuffer(s)\n", nf);

    for (i = 0; i < nf; i++) {
        struct image img;
        int shown = 0;

        for (j = 0; j < ns; j++)
            if (s[j].fb_id == fbs[i] && !s[j].crop)
                shown = 1;
        for (j = 0; j < np; j++)
            if (p[j].fb_id == fbs[i] && p[j].crtc_id)
                shown = 1;
        if (shown)
            continue;

        memset(&img, 0, sizeof(img));
        quiet = 1;
        if (drm_capture_fb(fd, fbs[i], NULL, &img, 0) == 0 &&
            rotate_image(&img, turns) == 0) {
            snprintf(img.label, sizeof(img.label), "fb%u", fbs[i]);
            write_named(on, img.label, &img);
        }
        quiet = 0;
        free(img.rgb);
    }
}

int main(int argc, char **argv)
{
    struct image imgs[MAX_SCREENS], out;
    struct screen scr[MAX_SCREENS];
    struct plane planes[MAX_PLANES];
    struct screen_pick want[MAX_SCREENS];
    const char *dev = NULL;
    struct outname on;
    int nwant = 0, reverse = 0, separate = 0, all = 0, list = 0, rot = -1;
    int fd, n = 0, ns = 0, np = 0, opt, i, turns, ret = 0;

    prog_name = "knulli-screenshot";
    memset(imgs, 0, sizeof(imgs));
    memset(&out, 0, sizeof(out));

    while ((opt = getopt(argc, argv, "d:c:rR:salvh")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'c':
            if (parse_crtc_list(optarg, want, &nwant) != 0)
                return 2;
            break;
        case 'r': reverse = 1; break;
        case 'R':
            if (optarg[0] < '0' || optarg[0] > '3' || optarg[1]) {
                err("rotation must be 0-3\n");
                return 2;
            }
            rot = optarg[0] - '0';
            break;
        case 's': separate = 1; break;
        case 'a': all = 1; break;
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
    if (argc - optind > 1) {
        usage(stderr);
        return 2;
    }

    fd = drm_open(dev);
    if (list) {
        if (fd >= 0) {
            drm_list(fd);
            close(fd);
        } else if (dev || fbdev_list() != 0) {
            err("no usable display found\n");
            return 1;
        }
        return 0;
    }

    /* Name prefix: given path minus ".png", or the default plus a timestamp. */
    memset(&on, 0, sizeof(on));
    if (optind < argc && argv[optind][0]) {
        size_t len = strlen(argv[optind]);

        if (len >= sizeof(on.prefix)) {
            err("path too long\n");
            return 2;
        }
        strcpy(on.prefix, argv[optind]);
        if (len > 4 && strcasecmp(on.prefix + len - 4, ".png") == 0)
            on.prefix[len - 4] = '\0';
    } else {
        time_t now = time(NULL);
        struct tm tm;

        localtime_r(&now, &tm);
        strftime(on.stamp, sizeof(on.stamp), "%Y%m%d_%Hh%Mm%Ss", &tm);
        snprintf(on.prefix, sizeof(on.prefix), "%s", DEFAULT_DIR "/" DEFAULT_PREFIX);
    }
    make_parent_dirs(on.prefix);

    if (fd >= 0) {
        np = drm_get_planes(fd, planes, MAX_PLANES);
        ns = drm_get_screens(fd, scr, MAX_SCREENS, planes, np);
        if (ns > 0)
            ns = select_screens(scr, ns, want, nwant, reverse);
        for (i = 0; i < ns; i++) {
            uint32_t crop[4] = { scr[i].x, scr[i].y, scr[i].mode_w, scr[i].mode_h };

            /* All planes composited; failing that, the primary fb alone. */
            if (drm_compose_screen(fd, &scr[i], planes, np, &imgs[n]) == 0 ||
                (scr[i].fb_id &&
                 drm_capture_fb(fd, scr[i].fb_id, scr[i].crop ? crop : NULL,
                                &imgs[n], 0) == 0)) {
                snprintf(imgs[n].label, sizeof(imgs[n].label), "%s",
                         scr[i].name);
                logv("%s (CRTC %u) captured %ux%u\n", scr[i].name,
                     scr[i].crtc_id, imgs[n].w, imgs[n].h);
                n++;
            } else if (nwant > 0) {
                free_images(imgs, n);
                close(fd);
                return 1;
            }
        }
    }

    if (n == 0) {
        if (dev || nwant > 0) {
            err("DRM capture failed\n");
            if (fd >= 0)
                close(fd);
            return 1;
        }
        logv("DRM capture unavailable, trying /dev/fb0\n");
        if (fbdev_capture("/dev/fb0", &imgs[0]) != 0) {
            err("no usable display found\n");
            if (fd >= 0)
                close(fd);
            return 1;
        }
        strcpy(imgs[0].label, "fb0");
        n = 1;
    }

    /* The picture was turned `rot` for the panel; turn it back upright. */
    if (rot < 0)
        rot = panel_rotation();
    turns = (4 - rot) & 3;
    if (turns)
        logv("panel rotation %d: turning capture %d quarter turn(s) clockwise\n",
             rot, turns);
    for (i = 0; i < n; i++)
        if (rotate_image(&imgs[i], turns) != 0) {
            free_images(imgs, n);
            if (fd >= 0)
                close(fd);
            return 1;
        }

    if (separate) {
        for (i = 0; i < n; i++)
            if (write_named(&on, imgs[i].label, &imgs[i]) != 0)
                ret = 1;
    } else if (n == 1) {
        ret = write_named(&on, NULL, &imgs[0]) != 0;
    } else if (stack_images(imgs, n, &out) == 0) {
        ret = write_named(&on, NULL, &out) != 0;
        free(out.rgb);
    } else {
        ret = 1;
    }
    free_images(imgs, n);

    if (all && fd >= 0)
        dump_extra_fbs(fd, scr, ns, planes, np, &on, turns);
    if (fd >= 0)
        close(fd);
    return ret;
}
