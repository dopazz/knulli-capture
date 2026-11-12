/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mkv.h - minimal Matroska writer for knulli-record: one H.264 video track
 * and optionally one audio track (Opus or 16-bit PCM).
 */
#ifndef MKV_H
#define MKV_H

#include <stddef.h>
#include <stdint.h>

enum { MKV_VIDEO = 1, MKV_AUDIO = 2 }; /* track numbers */

struct mkv_video {
    int width, height;
    const uint8_t *avcc; /* AVCDecoderConfigurationRecord */
    size_t avcc_len;
};

struct mkv_audio {
    int rate, channels;
    int opus;             /* 1: Opus, 0: 16-bit little-endian PCM */
    const uint8_t *head;  /* OpusHead, for Opus */
    size_t head_len;
    uint64_t delay_ns;    /* Opus pre-skip */
};

struct mkv;

/* `audio` may be NULL for a video-only file. */
struct mkv *mkv_open(const char *path, const struct mkv_video *video,
                     const struct mkv_audio *audio);
/* Add a frame; `ts_ms` should not run backwards much within a track. */
int mkv_write(struct mkv *m, int track, int64_t ts_ms, int keyframe,
              const uint8_t *data, size_t len);
/* Finish the file (index, duration) and close it. */
int mkv_close(struct mkv *m, int64_t duration_ms);

#endif
