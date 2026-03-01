// SPDX-License-Identifier: MIT
#ifndef LIBRA_AUDIO_H
#define LIBRA_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#define LIBRA_RING_FRAMES 16384  /* 64 KB stereo int16 */

typedef struct {
    int16_t  buf[LIBRA_RING_FRAMES * 2]; /* interleaved L/R */
    unsigned write_pos;
    unsigned read_pos;
    unsigned count;   /* frames currently in buffer */

    /* Linear resampler state */
    unsigned dst_rate;     /* target output rate Hz */
    double   ratio;        /* src_rate / dst_rate (working, possibly adjusted) */
    double   base_ratio;   /* fixed src_rate / dst_rate, set at init */
    double   frac;         /* fractional position [0, 1) */
    int16_t  prev[2];      /* last sample from previous flush (L, R) */

    unsigned target_queue_frames; /* target SDL queue depth (stereo frames); 0 = disabled */
} libra_audio_t;

libra_audio_t *libra_audio_create(double src_rate, unsigned dst_rate);
void           libra_audio_destroy(libra_audio_t *a);

void libra_audio_push(libra_audio_t *a, int16_t left, int16_t right);
void libra_audio_push_batch(libra_audio_t *a, const int16_t *data, size_t frames);

/* Resample and call the host audio callback */
typedef void (*libra_audio_output_fn)(void *ud, const int16_t *data, size_t frames);
void libra_audio_flush(libra_audio_t *a,
                       libra_audio_output_fn fn, void *userdata);

/* Update source rate (e.g. after SET_SYSTEM_AV_INFO) */
void libra_audio_set_src_rate(libra_audio_t *a, double src_rate);

/* Proportional rate adjustment based on SDL audio queue depth.
 * queue_bytes: current SDL queue size in bytes.
 * frame_size: bytes per stereo frame (typically 4 for int16 stereo). */
void libra_audio_adjust_rate(libra_audio_t *a, unsigned queue_bytes,
                              unsigned frame_size);

#endif /* LIBRA_AUDIO_H */
