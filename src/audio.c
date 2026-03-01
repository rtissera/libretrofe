// SPDX-License-Identifier: MIT
#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

libra_audio_t *libra_audio_create(double src_rate, unsigned dst_rate)
{
    libra_audio_t *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->dst_rate   = dst_rate;
    a->base_ratio = src_rate / (double)dst_rate;
    a->ratio      = a->base_ratio;
    return a;
}

void libra_audio_destroy(libra_audio_t *a)
{
    free(a);
}

void libra_audio_set_src_rate(libra_audio_t *a, double src_rate)
{
    if (!a || a->dst_rate == 0)
        return;
    a->base_ratio = src_rate / (double)a->dst_rate;
    a->ratio      = a->base_ratio;
}

void libra_audio_push(libra_audio_t *a, int16_t left, int16_t right)
{
    if (a->count >= LIBRA_RING_FRAMES)
        return; /* overflow: drop oldest would be better, but keep it simple */

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

void libra_audio_flush(libra_audio_t *a,
                       libra_audio_output_fn fn, void *userdata)
{
    if (!a->count || !fn)
        return;

    /* Estimate output frames: count / ratio (we produce fewer if upsampling) */
    unsigned in_frames  = a->count;
    size_t   out_max    = (size_t)(in_frames / a->ratio) + 2;
    int16_t *out        = malloc(out_max * 2 * sizeof(int16_t));
    if (!out)
        return;

    size_t   out_count  = 0;
    double   frac       = a->frac;
    int16_t  prev_l     = a->prev[0];
    int16_t  prev_r     = a->prev[1];

    /* Current read position in ring buffer */
    unsigned rpos = a->read_pos;
    unsigned avail = in_frames;

    /* We advance frac by ratio per output sample.
     * When frac >= 1 we consume an input sample. */
    while (out_count < out_max) {
        /* Interpolate between prev and current input sample */
        int16_t cur_l, cur_r;

        if (avail == 0 && frac < 1.0) {
            /* No more input; we may be able to produce a few more output
             * samples using the last pair, but stop to avoid unbounded loop. */
            break;
        }

        /* Get the current (next) input sample if frac >= 1 */
        while (frac >= 1.0) {
            if (avail == 0)
                break;
            prev_l = a->buf[rpos * 2];
            prev_r = a->buf[rpos * 2 + 1];
            rpos   = (rpos + 1) % LIBRA_RING_FRAMES;
            avail--;
            frac -= 1.0;
        }

        if (avail == 0) {
            /* Use prev as both endpoints when no more data */
            cur_l = prev_l;
            cur_r = prev_r;
        } else {
            cur_l = a->buf[rpos * 2];
            cur_r = a->buf[rpos * 2 + 1];
        }

        /* Linear interpolation */
        double t = frac;
        out[out_count * 2]     = (int16_t)(prev_l + t * (cur_l - prev_l));
        out[out_count * 2 + 1] = (int16_t)(prev_r + t * (cur_r - prev_r));
        out_count++;

        frac += a->ratio;
    }

    /* Save resampler state for next flush */
    a->frac    = frac - floor(frac); /* keep fractional part */
    a->prev[0] = prev_l;
    a->prev[1] = prev_r;

    /* Consume all input frames that were fully processed */
    a->read_pos = rpos;
    a->count    = avail;
    /* Adjust write_pos to stay consistent */
    if (a->count == 0)
        a->write_pos = a->read_pos = 0;

    if (out_count > 0)
        fn(userdata, out, out_count);

    free(out);
}

void libra_audio_adjust_rate(libra_audio_t *a, unsigned queue_bytes,
                              unsigned frame_size)
{
    if (!a || a->target_queue_frames == 0 || frame_size == 0)
        return;

    unsigned queue_frames = queue_bytes / frame_size;
    double target = (double)a->target_queue_frames;
    double error  = ((double)queue_frames - target) / target;

    double adjusted = a->base_ratio * (1.0 + error * 0.005);

    /* Clamp to +/-5% of base ratio */
    double lo = a->base_ratio * 0.95;
    double hi = a->base_ratio * 1.05;
    if (adjusted < lo) adjusted = lo;
    if (adjusted > hi) adjusted = hi;

    a->ratio = adjusted;
}
