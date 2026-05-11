// SPDX-License-Identifier: MIT
#ifndef LIBRA_AUDIO_H
#define LIBRA_AUDIO_H

#include <stdint.h>
#include <stddef.h>

/* Ring buffer holds 16384 stereo int16 frames (= 64 KB). */
#define LIBRA_RING_FRAMES 16384

/* Hard cap on the preallocated resampler output buffer.
 * 4 × LIBRA_RING_FRAMES stereo frames = 256 KB.  Enough for ratios down
 * to ~0.25 (4× upsample); degenerate beyond that, output truncates. */
#define LIBRA_OUT_FRAMES_MAX (LIBRA_RING_FRAMES * 4)

typedef struct {
    int16_t  buf[LIBRA_RING_FRAMES * 2]; /* interleaved L/R */
    unsigned write_pos;
    unsigned read_pos;
    unsigned count;   /* frames currently in buffer */

    /* Q32 phase, Q16 interp linear resampler.
     *
     * The phase counter (`ratio_q32`, `frac_q32`) uses 32-bit fractional
     * precision so that an irrational ratio (e.g. 22050/48000) doesn't
     * drift audibly over thousands of samples — Q16.16 phase showed up
     * to 55 LSB error against the double reference after 4000 iters;
     * Q32 brings that under 1 LSB.
     *
     * The interpolation itself stays Q16 (`(delta * frac_q16) >> 16`)
     * — that's already at 16-bit input precision, no benefit from more.
     *
     *   ratio_q32 = round(src_rate / dst_rate * 2^32)
     *   frac_q32 += ratio_q32 each output; consume input on overflow.
     *   frac_q16 = frac_q32 >> 16   (upper 16 bits → 0..0xFFFF)
     *   out[n]   = prev + ((cur - prev) * frac_q16) >> 16
     */
    unsigned dst_rate;
    uint64_t base_ratio_q32;   /* fixed src_rate / dst_rate at init    */
    uint64_t ratio_q32;        /* working — adjusted by adjust_rate    */
    uint64_t frac_q32;         /* phase position [0..2^32)             */
    int16_t  prev[2];          /* last consumed sample (L, R)          */

    /* Preallocated resampler output scratch.  Sized once at create()
     * based on the worst-case ratio under adjust_rate's ±5% clamp.
     * Never reallocated during normal operation; grows only when
     * libra_audio_set_src_rate changes the base ratio so dramatically
     * that the existing cap no longer suffices. */
    int16_t *out_buf;
    size_t   out_cap_frames;   /* allocated capacity in stereo frames  */

    unsigned target_queue_frames; /* target SDL queue depth; 0 = disabled */
} libra_audio_t;

libra_audio_t *libra_audio_create(double src_rate, unsigned dst_rate);
void           libra_audio_destroy(libra_audio_t *a);

void libra_audio_push(libra_audio_t *a, int16_t left, int16_t right);
void libra_audio_push_batch(libra_audio_t *a, const int16_t *data, size_t frames);

/* Resample and call the host audio callback */
typedef void (*libra_audio_output_fn)(void *ud, const int16_t *data, size_t frames);
void libra_audio_flush(libra_audio_t *a,
                       libra_audio_output_fn fn, void *userdata);

/* Update source rate (e.g. after SET_SYSTEM_AV_INFO) — may reallocate
 * the output scratch if the new ratio needs more headroom (bounded by
 * LIBRA_OUT_FRAMES_MAX). */
void libra_audio_set_src_rate(libra_audio_t *a, double src_rate);

/* Proportional rate adjustment based on SDL audio queue depth.
 *   queue_bytes: current SDL queue size in bytes.
 *   frame_size:  bytes per stereo frame (typically 4 for int16 stereo).
 * Result is clamped to base_ratio * [0.95, 1.05] so the preallocated
 * output buffer always has enough room. */
void libra_audio_adjust_rate(libra_audio_t *a, unsigned queue_bytes,
                              unsigned frame_size);

#endif /* LIBRA_AUDIO_H */
