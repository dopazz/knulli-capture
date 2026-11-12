/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mp4.h - minimal MP4 writer for knulli-record: one H.264 video track and
 * optionally one AAC audio track. Same track numbers as mkv.h.
 */
#ifndef MP4_H
#define MP4_H

#include <stddef.h>
#include <stdint.h>

enum { MP4_VIDEO = 1, MP4_AUDIO = 2 }; /* track ids */

struct mp4_video {
    int width, height;
    const uint8_t *avcc;  /* AVCDecoderConfigurationRecord */
    size_t avcc_len;
};

struct mp4_audio {
    int rate, channels;
    int frame;            /* samples per channel in each packet (1024) */
    int delay;            /* encoder delay in samples, trimmed on playback */
    int bitrate;          /* bits/s, for the decoder config */
    const uint8_t *asc;   /* AudioSpecificConfig */
    size_t asc_len;
};

struct mp4;

/* `audio` may be NULL for a video-only file. */
struct mp4 *mp4_open(const char *path, const struct mp4_video *video,
                     const struct mp4_audio *audio);
/* Add a frame; `ts_ms` must not run backwards within a track. */
int mp4_write(struct mp4 *m, int track, int64_t ts_ms, int keyframe,
              const uint8_t *data, size_t len);
/* Write the index (moov) and close the file. Until then the file does not
 * play: unlike the MKV writer, a recording cut short is lost. */
int mp4_close(struct mp4 *m, int64_t duration_ms);

#endif
