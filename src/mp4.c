/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mp4.c - minimal MP4 (ISO base media) writer for knulli-record.
 *
 * Samples go straight into one mdat as they arrive; their sizes, offsets
 * and times are kept in memory (~24 bytes each, ~7 MB for an hour) and the
 * index (moov) is written after the mdat on close, the layout ffmpeg
 * writes by default. Each sample is its own chunk, which keeps the tables
 * simple at the cost of a slightly larger index.
 *
 * Timing: video uses a 90 kHz clock, audio the sample rate. Each track's
 * edit list starts it at its first timestamp, so audio and video stay in
 * sync whichever starts first, and skips the AAC encoder's delay.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mp4.h"

#define MOVIE_SCALE 1000      /* movie (and edit list) time unit: ms */
#define VIDEO_SCALE 90000

struct buf {
    uint8_t *d;
    size_t n, cap;
    int oom;
};

struct sample {
    uint64_t off;
    int64_t ts;               /* ms */
    uint32_t size;
    uint32_t key;
};

struct track {
    struct sample *s;
    size_t n, cap;
};

struct mp4 {
    FILE *f;
    uint64_t pos;             /* bytes written so far */
    uint64_t mdat_pos;        /* file offset of the mdat header */
    struct mp4_video v;
    struct mp4_audio a;
    uint8_t *avcc, *asc;      /* copies */
    int has_audio;
    struct track t[3];        /* by track id */
    int err;
};

static void need(struct buf *b, size_t n)
{
    size_t cap;
    uint8_t *d;

    if (b->n + n <= b->cap || b->oom)
        return;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + n)
        cap *= 2;
    d = realloc(b->d, cap);
    if (!d) {
        b->oom = 1;
        return;
    }
    b->d = d;
    b->cap = cap;
}

static void put(struct buf *b, const void *p, size_t n)
{
    need(b, n);
    if (b->oom)
        return;
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void put8(struct buf *b, uint32_t v)
{
    uint8_t c = (uint8_t)v;

    put(b, &c, 1);
}

static void put16(struct buf *b, uint32_t v)
{
    put8(b, v >> 8);
    put8(b, v);
}

static void put24(struct buf *b, uint32_t v)
{
    put8(b, v >> 16);
    put16(b, v);
}

static void put32(struct buf *b, uint32_t v)
{
    put16(b, v >> 16);
    put16(b, v);
}

static void put64(struct buf *b, uint64_t v)
{
    put32(b, (uint32_t)(v >> 32));
    put32(b, (uint32_t)v);
}

static void put_zero(struct buf *b, size_t n)
{
    while (n--)
        put8(b, 0);
}

/* Box with its size filled in by box_end(). */
static size_t box(struct buf *b, const char *type)
{
    size_t at = b->n;

    put32(b, 0);
    put(b, type, 4);
    return at;
}

static size_t fullbox(struct buf *b, const char *type, int version,
                      uint32_t flags)
{
    size_t at = box(b, type);

    put8(b, (uint32_t)version);
    put24(b, flags);
    return at;
}

static void box_end(struct buf *b, size_t at)
{
    uint32_t n = (uint32_t)(b->n - at);

    if (b->oom)
        return;
    b->d[at] = (uint8_t)(n >> 24);
    b->d[at + 1] = (uint8_t)(n >> 16);
    b->d[at + 2] = (uint8_t)(n >> 8);
    b->d[at + 3] = (uint8_t)n;
}

static void put_matrix(struct buf *b)
{
    static const uint32_t m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0,
                                   0, 0, 0x40000000 };
    int i;

    for (i = 0; i < 9; i++)
        put32(b, m[i]);
}

static int write_raw(struct mp4 *m, const void *p, size_t n)
{
    if (m->err || fwrite(p, 1, n, m->f) != n)
        m->err = 1;
    m->pos += n;
    return m->err ? -1 : 0;
}

static void buf_free(struct buf *b)
{
    free(b->d);
    memset(b, 0, sizeof(*b));
}

struct mp4 *mp4_open(const char *path, const struct mp4_video *video,
                     const struct mp4_audio *audio)
{
    struct buf b = { 0 };
    struct mp4 *m = calloc(1, sizeof(*m));
    size_t at;

    if (!m)
        return NULL;
    m->v = *video;
    m->avcc = malloc(video->avcc_len);
    if (audio) {
        m->a = *audio;
        m->has_audio = 1;
        m->asc = malloc(audio->asc_len);
    }
    m->f = fopen(path, "wb");
    if (!m->f || !m->avcc || (audio && !m->asc))
        goto fail;
    memcpy(m->avcc, video->avcc, video->avcc_len);
    m->v.avcc = m->avcc;
    if (audio) {
        memcpy(m->asc, audio->asc, audio->asc_len);
        m->a.asc = m->asc;
    }
    setvbuf(m->f, NULL, _IOFBF, 256 * 1024);

    at = box(&b, "ftyp");
    put(&b, "isom", 4);
    put32(&b, 0x200);
    put(&b, "isomiso2avc1mp41", 16);
    box_end(&b, at);
    /* mdat with a 64-bit size, filled in on close */
    m->mdat_pos = b.n;
    put32(&b, 1);
    put(&b, "mdat", 4);
    put64(&b, 0);
    if (b.oom)
        goto fail;
    write_raw(m, b.d, b.n);
    buf_free(&b);
    if (m->err)
        goto fail;
    return m;

fail:
    buf_free(&b);
    if (m->f)
        fclose(m->f);
    free(m->avcc);
    free(m->asc);
    free(m);
    return NULL;
}

int mp4_write(struct mp4 *m, int track, int64_t ts_ms, int keyframe,
              const uint8_t *data, size_t len)
{
    struct track *t;
    struct sample *s;

    if (track != MP4_VIDEO && (track != MP4_AUDIO || !m->has_audio))
        return -1;
    t = &m->t[track];
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        struct sample *ns = realloc(t->s, cap * sizeof(*ns));

        if (!ns) {
            m->err = 1;
            return -1;
        }
        t->s = ns;
        t->cap = cap;
    }
    /* Timestamps must not run backwards within a track. */
    if (t->n && ts_ms < t->s[t->n - 1].ts)
        ts_ms = t->s[t->n - 1].ts;
    s = &t->s[t->n++];
    s->off = m->pos;
    s->ts = ts_ms;
    s->size = (uint32_t)len;
    s->key = keyframe ? 1 : 0;
    return write_raw(m, data, len);
}

/* Duration of each sample in the track's own time units. */
static uint32_t *durations(struct mp4 *m, int track, int64_t duration_ms,
                           uint64_t *total)
{
    struct track *t = &m->t[track];
    uint32_t *d = malloc((t->n ? t->n : 1) * sizeof(*d));
    size_t i;

    *total = 0;
    if (!d)
        return NULL;
    for (i = 0; i < t->n; i++) {
        int64_t gap = i + 1 < t->n ? t->s[i + 1].ts - t->s[i].ts : -1;

        if (track == MP4_VIDEO) {
            /* The last frame stays up until the recording ends. */
            if (gap < 0)
                gap = duration_ms > t->s[i].ts ? duration_ms - t->s[i].ts
                    : i ? (int64_t)d[i - 1] / (VIDEO_SCALE / 1000) : 33;
            d[i] = (uint32_t)(gap > 0 ? gap : 1) * (VIDEO_SCALE / 1000);
        } else {
            /* Audio packets follow on from each other; a real gap (the
             * stream stalled) is kept so later audio stays in sync. */
            int64_t g = gap * m->a.rate / 1000;

            d[i] = (uint32_t)m->a.frame;
            if (gap > 0 && g > m->a.frame + m->a.rate / 10)
                d[i] = (uint32_t)g;
        }
        *total += d[i];
    }
    return d;
}

/* edts: start the track at `start_ms`, play `len_ms` from `media_time`. */
static void put_edits(struct buf *b, int64_t start_ms, int64_t len_ms,
                      int64_t media_time)
{
    size_t edts = box(b, "edts"), elst = fullbox(b, "elst", 0, 0);

    put32(b, start_ms > 0 ? 2 : 1);
    if (start_ms > 0) {
        put32(b, (uint32_t)start_ms);    /* an empty edit: nothing plays */
        put32(b, 0xffffffff);
        put32(b, 0x00010000);
    }
    put32(b, (uint32_t)len_ms);
    put32(b, (uint32_t)media_time);
    put32(b, 0x00010000);              /* rate 1.0 */
    box_end(b, elst);
    box_end(b, edts);
}

static void put_sample_entry_video(struct buf *b, const struct mp4_video *v)
{
    size_t e = box(b, "avc1"), c;
    uint8_t name[32] = { 0 };

    put_zero(b, 6);
    put16(b, 1);                       /* data reference index */
    put_zero(b, 16);
    put16(b, (uint32_t)v->width);
    put16(b, (uint32_t)v->height);
    put32(b, 0x00480000);              /* 72 dpi */
    put32(b, 0x00480000);
    put32(b, 0);
    put16(b, 1);                       /* frames per sample */
    put(b, name, sizeof(name));        /* compressor name */
    put16(b, 0x18);                    /* depth */
    put16(b, 0xffff);
    c = box(b, "avcC");
    put(b, v->avcc, v->avcc_len);
    box_end(b, c);
    box_end(b, e);
}

static void put_sample_entry_audio(struct buf *b, const struct mp4_audio *a,
                                   int track_id)
{
    size_t e = box(b, "mp4a"), es;

    put_zero(b, 6);
    put16(b, 1);                       /* data reference index */
    put_zero(b, 8);
    put16(b, (uint32_t)a->channels);
    put16(b, 16);                      /* sample size */
    put_zero(b, 4);
    put32(b, (uint32_t)a->rate << 16);
    /* esds: ES descriptor > decoder config > AudioSpecificConfig */
    es = fullbox(b, "esds", 0, 0);
    put8(b, 0x03);
    put8(b, (uint32_t)(3 + 2 + 13 + 2 + a->asc_len + 3));
    put16(b, (uint32_t)track_id);
    put8(b, 0);
    put8(b, 0x04);
    put8(b, (uint32_t)(13 + 2 + a->asc_len));
    put8(b, 0x40);                     /* MPEG-4 audio */
    put8(b, 0x15);                     /* audio stream */
    put24(b, (uint32_t)(768 * a->channels)); /* decoder buffer */
    put32(b, (uint32_t)a->bitrate);
    put32(b, (uint32_t)a->bitrate);
    put8(b, 0x05);
    put8(b, (uint32_t)a->asc_len);
    put(b, a->asc, a->asc_len);
    put8(b, 0x06);                     /* SL config: MP4 */
    put8(b, 1);
    put8(b, 2);
    box_end(b, es);
    box_end(b, e);
}

/* trak; returns its duration in ms as shown (edits included). */
static int64_t put_trak(struct mp4 *m, struct buf *b, int track,
                        int64_t duration_ms)
{
    struct track *t = &m->t[track];
    int video = track == MP4_VIDEO;
    uint32_t scale = video ? VIDEO_SCALE : (uint32_t)m->a.rate;
    uint64_t media = 0;
    uint32_t *d = durations(m, track, duration_ms, &media);
    int64_t start = t->s[0].ts, skip = video ? 0 : m->a.delay, len;
    size_t trak, x, y, z, i, j;
    int big = t->s[t->n - 1].off > 0xffffffffULL;

    if (!d) {
        m->err = 1;
        return 0;
    }
    if ((int64_t)media <= skip)
        skip = 0;
    len = (int64_t)((media - (uint64_t)skip) * MOVIE_SCALE / scale);

    trak = box(b, "trak");
    x = fullbox(b, "tkhd", 0, 3);      /* enabled, in movie */
    put32(b, 0);
    put32(b, 0);
    put32(b, (uint32_t)track);
    put32(b, 0);
    put32(b, (uint32_t)(start + len));
    put_zero(b, 8);
    put16(b, 0);                       /* layer */
    put16(b, video ? 0 : 1);           /* alternate group */
    put16(b, video ? 0 : 0x0100);      /* volume */
    put16(b, 0);
    put_matrix(b);
    put32(b, video ? (uint32_t)m->v.width << 16 : 0);
    put32(b, video ? (uint32_t)m->v.height << 16 : 0);
    box_end(b, x);

    put_edits(b, start, len, skip);

    x = box(b, "mdia");
    if (media > 0xffffffffULL) {
        y = fullbox(b, "mdhd", 1, 0);
        put64(b, 0);
        put64(b, 0);
        put32(b, scale);
        put64(b, media);
    } else {
        y = fullbox(b, "mdhd", 0, 0);
        put32(b, 0);
        put32(b, 0);
        put32(b, scale);
        put32(b, (uint32_t)media);
    }
    put16(b, 0x55c4);                  /* language "und" */
    put16(b, 0);
    box_end(b, y);
    y = fullbox(b, "hdlr", 0, 0);
    put32(b, 0);
    put(b, video ? "vide" : "soun", 4);
    put_zero(b, 12);
    put(b, video ? "VideoHandler" : "SoundHandler", 13);
    box_end(b, y);

    y = box(b, "minf");
    if (video) {
        z = fullbox(b, "vmhd", 0, 1);
        put_zero(b, 8);
    } else {
        z = fullbox(b, "smhd", 0, 0);
        put_zero(b, 4);
    }
    box_end(b, z);
    z = box(b, "dinf");
    {
        size_t dref = fullbox(b, "dref", 0, 0), url;

        put32(b, 1);
        url = fullbox(b, "url ", 0, 1); /* data in this file */
        box_end(b, url);
        box_end(b, dref);
    }
    box_end(b, z);

    z = box(b, "stbl");
    {
        size_t s = fullbox(b, "stsd", 0, 0);

        put32(b, 1);
        if (video)
            put_sample_entry_video(b, &m->v);
        else
            put_sample_entry_audio(b, &m->a, track);
        box_end(b, s);

        /* stts: runs of equal durations */
        s = fullbox(b, "stts", 0, 0);
        {
            size_t count_at = b->n, runs = 0;

            put32(b, 0);
            for (i = 0; i < t->n; i = j) {
                for (j = i + 1; j < t->n && d[j] == d[i]; j++)
                    ;
                put32(b, (uint32_t)(j - i));
                put32(b, d[i]);
                runs++;
            }
            if (!b->oom) {
                b->d[count_at] = (uint8_t)(runs >> 24);
                b->d[count_at + 1] = (uint8_t)(runs >> 16);
                b->d[count_at + 2] = (uint8_t)(runs >> 8);
                b->d[count_at + 3] = (uint8_t)runs;
            }
        }
        box_end(b, s);

        if (video) {
            size_t n = 0;

            for (i = 0; i < t->n; i++)
                n += t->s[i].key;
            s = fullbox(b, "stss", 0, 0);
            put32(b, (uint32_t)n);
            for (i = 0; i < t->n; i++)
                if (t->s[i].key)
                    put32(b, (uint32_t)(i + 1));
            box_end(b, s);
        }

        s = fullbox(b, "stsc", 0, 0);  /* one sample per chunk */
        put32(b, 1);
        put32(b, 1);
        put32(b, 1);
        put32(b, 1);
        box_end(b, s);

        s = fullbox(b, "stsz", 0, 0);
        put32(b, 0);
        put32(b, (uint32_t)t->n);
        for (i = 0; i < t->n; i++)
            put32(b, t->s[i].size);
        box_end(b, s);

        s = fullbox(b, big ? "co64" : "stco", 0, 0);
        put32(b, (uint32_t)t->n);
        for (i = 0; i < t->n; i++) {
            if (big)
                put64(b, t->s[i].off);
            else
                put32(b, (uint32_t)t->s[i].off);
        }
        box_end(b, s);
    }
    box_end(b, z);
    box_end(b, y);
    box_end(b, x);
    box_end(b, trak);
    free(d);
    return start + len;
}

int mp4_close(struct mp4 *m, int64_t duration_ms)
{
    struct buf b = { 0 }, tracks = { 0 };
    int64_t movie = 0, d;
    uint64_t mdat_size = m->pos - m->mdat_pos;
    size_t moov, x;
    int ret, i, next_id = 1;

    for (i = MP4_VIDEO; i <= MP4_AUDIO; i++) {
        if (!m->t[i].n)
            continue;
        d = put_trak(m, &tracks, i, duration_ms);
        if (d > movie)
            movie = d;
        next_id = i + 1;
    }

    moov = box(&b, "moov");
    x = fullbox(&b, "mvhd", 0, 0);
    put32(&b, 0);
    put32(&b, 0);
    put32(&b, MOVIE_SCALE);
    put32(&b, (uint32_t)movie);
    put32(&b, 0x00010000);             /* rate 1.0 */
    put16(&b, 0x0100);                 /* volume 1.0 */
    put_zero(&b, 10);
    put_matrix(&b);
    put_zero(&b, 24);
    put32(&b, (uint32_t)next_id);
    box_end(&b, x);
    put(&b, tracks.d, tracks.n);
    if (tracks.oom)
        b.oom = 1;
    box_end(&b, moov);

    if (b.oom)
        m->err = 1;
    else
        write_raw(m, b.d, b.n);
    buf_free(&b);
    buf_free(&tracks);

    /* The mdat's real size. */
    if (!m->err && fseeko(m->f, (off_t)m->mdat_pos + 8, SEEK_SET) == 0) {
        uint8_t s[8];

        for (i = 0; i < 8; i++)
            s[i] = (uint8_t)(mdat_size >> (8 * (7 - i)));
        if (fwrite(s, 1, 8, m->f) != 8)
            m->err = 1;
    }

    if (fflush(m->f) != 0)
        m->err = 1;
    fsync(fileno(m->f));
    ret = (fclose(m->f) != 0 || m->err) ? -1 : 0;
    for (i = 0; i < 3; i++)
        free(m->t[i].s);
    free(m->avcc);
    free(m->asc);
    free(m);
    return ret;
}
