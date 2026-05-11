// SPDX-License-Identifier: MIT
#include "audio.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Compute the worst-case output frame count for one libra_audio_flush()
 * given a base ratio.  adjust_rate() clamps the working ratio to
 * base_ratio * [0.95, 1.05]; the minimum ratio determines the maximum
 * output count for a full ring buffer of input.
 *
 * Hard-capped at LIBRA_OUT_FRAMES_MAX to bound memory at 256 KB even
 * if a core advertises a pathologically high dst_rate / low src_rate. */
static size_t worst_out_frames(uint64_t base_ratio_q32)
{
    if (base_ratio_q32 == 0) base_ratio_q32 = 1;
    uint64_t min_ratio = (base_ratio_q32 * 95) / 100;
    if (min_ratio == 0) min_ratio = 1;
    /* out_max = RING * 2^32 / min_ratio + 8.  Use 128-bit-ish trick:
     * RING fits in 16 bits, 2^32 in 33 bits; product fits in uint64. */
    uint64_t numer = (uint64_t)LIBRA_RING_FRAMES << 32;
    size_t   worst = (size_t)(numer / min_ratio) + 8;
    if (worst > LIBRA_OUT_FRAMES_MAX) worst = LIBRA_OUT_FRAMES_MAX;
    return worst;
}

/* Resize the output scratch to hold at least `frames` stereo samples.
 * Never shrinks (the buffer is small enough that the high-water mark
 * doesn't bloat the process; the goal is to avoid per-flush heap churn,
 * not to minimise peak RSS). */
static int audio_ensure_out_capacity(libra_audio_t *a, size_t frames)
{
    if (a->out_cap_frames >= frames) return 1;
    if (frames > LIBRA_OUT_FRAMES_MAX) frames = LIBRA_OUT_FRAMES_MAX;
    int16_t *nb = realloc(a->out_buf, frames * 2 * sizeof(int16_t));
    if (!nb) return 0;
    a->out_buf        = nb;
    a->out_cap_frames = frames;
    return 1;
}

static uint64_t ratio_to_q32(double src_rate, unsigned dst_rate)
{
    if (dst_rate == 0 || src_rate <= 0.0) return 0;
    double q = (src_rate / (double)dst_rate) * 4294967296.0 + 0.5;
    if (q < 1.0)                       return 1;
    if (q > (double)((uint64_t)1 << 62)) return (uint64_t)1 << 62;
    return (uint64_t)q;
}

/* -------------------------------------------------------------------------
 * Create / destroy
 * ---------------------------------------------------------------------- */

libra_audio_t *libra_audio_create(double src_rate, unsigned dst_rate)
{
    if (dst_rate == 0 || src_rate <= 0.0)
        return NULL;

    libra_audio_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;

    a->dst_rate       = dst_rate;
    a->base_ratio_q32 = ratio_to_q32(src_rate, dst_rate);
    a->ratio_q32      = a->base_ratio_q32;

    if (!audio_ensure_out_capacity(a, worst_out_frames(a->base_ratio_q32))) {
        free(a);
        return NULL;
    }
    return a;
}

void libra_audio_destroy(libra_audio_t *a)
{
    if (!a) return;
    free(a->out_buf);
    free(a);
}

void libra_audio_set_src_rate(libra_audio_t *a, double src_rate)
{
    if (!a || a->dst_rate == 0 || src_rate <= 0.0) return;
    a->base_ratio_q32 = ratio_to_q32(src_rate, a->dst_rate);
    a->ratio_q32      = a->base_ratio_q32;
    /* Only grow — never shrink — to keep allocations rare. */
    audio_ensure_out_capacity(a, worst_out_frames(a->base_ratio_q32));
}

/* -------------------------------------------------------------------------
 * Push
 * ---------------------------------------------------------------------- */

void libra_audio_push(libra_audio_t *a, int16_t left, int16_t right)
{
    if (a->count >= LIBRA_RING_FRAMES) {
        /* Buffer full: overwrite the oldest sample so we stay current.
         * Old audio is stale; we want the latest samples to be heard. */
        a->read_pos = (a->read_pos + 1) % LIBRA_RING_FRAMES;
        a->count--;
    }

    unsigned pos = a->write_pos * 2;
    a->buf[pos]     = left;
    a->buf[pos + 1] = right;
    a->write_pos    = (a->write_pos + 1) % LIBRA_RING_FRAMES;
    a->count++;
}

void libra_audio_push_batch(libra_audio_t *a, const int16_t *data, size_t frames)
{
    for (size_t i = 0; i < frames; i++)
        libra_audio_push(a, data[i * 2], data[i * 2 + 1]);
}

/* -------------------------------------------------------------------------
 * Flush — resample and emit
 *
 * Fixed-point linear interpolation:
 *
 *   out = prev + ((cur - prev) * frac_q16) >> 16
 *
 * Worst-case operand magnitudes: |cur - prev| ≤ 65535 (signed int17),
 * frac_q16 ∈ [0, 0xFFFF].  Their product is up to ~4.29e9 which exceeds
 * INT32_MAX, so the multiply is done in int64 to be portable across
 * armhf, armv7, riscv64, x86_64, and mips32r2.  On Cortex-A53 this is a
 * single-cycle umull/smull; on mips32r2 libgcc emits a short helper.
 *
 * No per-flush heap traffic — the output buffer is preallocated in
 * create() based on the worst-case ratio under adjust_rate's clamp.
 * ---------------------------------------------------------------------- */

void libra_audio_flush(libra_audio_t *a,
                       libra_audio_output_fn fn, void *userdata)
{
    if (!a || !a->count || !fn || !a->out_buf)
        return;

    /* Per-flush soft cap mirrors the previous double-precision version:
     * roughly `in_frames / ratio + 2` output samples.  The preallocated
     * out_cap is much larger; we take the smaller so we don't emit
     * unbounded "tail" frames when the ring drains while frac is still
     * mid-step.  out_cap itself is the hard ceiling and protects against
     * pathological ratios. */
    const uint64_t ratio     = a->ratio_q32;
    const uint64_t WHOLE     = (uint64_t)1 << 32;
    const size_t   out_cap   = a->out_cap_frames;
    size_t         soft_cap  = (ratio > 0)
        ? (size_t)(((uint64_t)a->count << 32) / ratio) + 2
        : out_cap;
    if (soft_cap > out_cap) soft_cap = out_cap;

    size_t   out_count = 0;
    uint64_t frac      = a->frac_q32;
    int16_t  prev_l    = a->prev[0];
    int16_t  prev_r    = a->prev[1];
    unsigned rpos      = a->read_pos;
    unsigned avail     = a->count;
    int16_t *out       = a->out_buf;

    while (out_count < soft_cap) {
        /* Stop when we have no more input AND haven't accumulated enough
         * fractional progress for the consume loop to fire — mirrors the
         * previous version's `if (avail == 0 && frac < 1.0) break`. */
        if (avail == 0 && frac < WHOLE)
            break;

        /* Consume input while frac crosses whole-sample boundaries. */
        while (frac >= WHOLE) {
            if (avail == 0) break;
            prev_l = a->buf[rpos * 2];
            prev_r = a->buf[rpos * 2 + 1];
            rpos   = (rpos + 1) % LIBRA_RING_FRAMES;
            avail--;
            frac  -= WHOLE;
        }

        int16_t cur_l, cur_r;
        if (avail == 0) {
            cur_l = prev_l;
            cur_r = prev_r;
        } else {
            cur_l = a->buf[rpos * 2];
            cur_r = a->buf[rpos * 2 + 1];
        }

        /* Interp at Q16: 16-bit precision already saturates 16-bit
         * audio.  Use the top 16 bits of the Q32 phase. */
        uint32_t frac_q16 = (uint32_t)(frac >> 16);
        int32_t  dl  = (int32_t)cur_l - (int32_t)prev_l;
        int32_t  dr  = (int32_t)cur_r - (int32_t)prev_r;
        int32_t  ol  = (int32_t)prev_l + (int32_t)(((int64_t)dl * (int64_t)frac_q16) >> 16);
        int32_t  or_ = (int32_t)prev_r + (int32_t)(((int64_t)dr * (int64_t)frac_q16) >> 16);
        out[out_count * 2]     = (int16_t)ol;
        out[out_count * 2 + 1] = (int16_t)or_;
        out_count++;

        frac += ratio;
    }

    a->frac_q32 = frac;
    a->prev[0]  = prev_l;
    a->prev[1]  = prev_r;
    a->read_pos = rpos;
    a->count    = avail;
    if (a->count == 0)
        a->write_pos = a->read_pos = 0;

    if (out_count > 0)
        fn(userdata, out, out_count);
}

/* -------------------------------------------------------------------------
 * Adjust working rate from queue depth
 *
 * Proportional controller — same shape as the previous double-precision
 * version, restated in Q16.16.  Internally promoted to int64 for the
 * 5% step and clamp arithmetic; final value is a uint32_t.
 * ---------------------------------------------------------------------- */

void libra_audio_adjust_rate(libra_audio_t *a, unsigned queue_bytes,
                              unsigned frame_size)
{
    if (!a || a->target_queue_frames == 0 || frame_size == 0)
        return;

    /* This controller runs once per host audio flush — not in the
     * per-sample inner loop — so plain double is the most portable way
     * to keep precision without dragging __int128 (unsupported on
     * armhf/armv7-32/mips32r2) into a per-frame multiply.
     *
     * Inputs: queue_frames vs target. Output: ratio_q32 clamped to
     * base_ratio_q32 * [0.95, 1.05]. */
    unsigned queue_frames = queue_bytes / frame_size;
    double   base = (double)a->base_ratio_q32;
    double   err  = ((double)queue_frames - (double)a->target_queue_frames)
                  / (double)a->target_queue_frames;
    double   adjusted = base * (1.0 + err * 0.005);
    double   lo = base * 0.95;
    double   hi = base * 1.05;
    if (adjusted < lo)  adjusted = lo;
    if (adjusted > hi)  adjusted = hi;
    if (adjusted < 1.0) adjusted = 1.0;

    a->ratio_q32 = (uint64_t)adjusted;
}
