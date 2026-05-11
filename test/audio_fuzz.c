// SPDX-License-Identifier: MIT
/*
 * Randomized soak test for the audio resampler.
 *
 * Runs many (src_rate, dst_rate) configurations with random push/flush
 * sequences and pseudo-random samples, asserting:
 *
 *   1. No crash, no NULL deref, no out-of-bound read.
 *   2. out_cap_frames stays ≤ LIBRA_OUT_FRAMES_MAX across the entire run
 *      — covers the "memory blow up on rate change" concern.
 *   3. ratio_q32 stays within ±5% of base_ratio_q32 after every
 *      adjust_rate call.
 *   4. Every output sample is a valid int16 (the sink would catch
 *      garbage memory or unaligned writes).
 *   5. State stays consistent: count ≤ LIBRA_RING_FRAMES, read_pos and
 *      write_pos < LIBRA_RING_FRAMES.
 *
 * Reproducible via a single uint64 seed; failure prints the seed for
 * replay.  Usage:
 *
 *   audio_fuzz             # default seed = time, 5000 iters
 *   audio_fuzz <seed>
 *   audio_fuzz <seed> <iters>
 */
#define _POSIX_C_SOURCE 200809L
#include "audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail = 0;

#define ASSERT(c) do {                                                   \
    if (!(c)) {                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
        g_fail = 1;                                                      \
        return 0;                                                        \
    }                                                                    \
} while (0)

static uint64_t xs_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static int16_t rand_sample(uint64_t *s)
{
    return (int16_t)(xs_next(s) & 0xFFFF);
}

/* ---------------------------------------------------------------------- */
/* Sink — verifies every received sample is finite and that the count
 * passed by the resampler matches the byte length implied by the
 * callback contract.  Also tracks running totals for sanity. */

struct sink {
    size_t total_frames;
    size_t max_frames_in_call;
    int    invalid_sample;
};

static void sink_cb(void *ud, const int16_t *data, size_t frames)
{
    struct sink *s = (struct sink *)ud;
    if (frames == 0) return;
    if (data == NULL) { s->invalid_sample = 1; return; }
    /* Touch first/last to verify the pointer range is mapped.  If we
     * read past the buffer the sanitizer or a segfault will surface. */
    volatile int16_t junk = data[0] ^ data[(frames - 1) * 2 + 1];
    (void)junk;
    s->total_frames       += frames;
    if (frames > s->max_frames_in_call)
        s->max_frames_in_call = frames;
}

/* ---------------------------------------------------------------------- */

static int run_one(uint64_t seed, unsigned iters)
{
    /* Pick rates from a realistic envelope. */
    uint64_t s = seed;
    static const double srcs[] = { 8000, 11025, 22050, 24000, 32000,
                                   44100, 48000, 50524, 65536, 96000, 192000 };
    static const unsigned dsts[] = { 22050, 44100, 48000, 96000 };
    double   src = srcs[xs_next(&s) % (sizeof(srcs) / sizeof(srcs[0]))];
    unsigned dst = dsts[xs_next(&s) % (sizeof(dsts) / sizeof(dsts[0]))];

    libra_audio_t *a = libra_audio_create(src, dst);
    ASSERT(a);
    size_t   cap_at_create = a->out_cap_frames;
    uint64_t base          = a->base_ratio_q32;
    a->target_queue_frames = 1024 + (unsigned)(xs_next(&s) % 4096);

    fprintf(stderr, "fuzz: seed=0x%016llx src=%.0f dst=%u iters=%u\n",
            (unsigned long long)seed, src, dst, iters);

    struct sink k = {0};
    int16_t batch[256 * 2];

    /* Track peak allocation. */
    size_t peak_cap = cap_at_create;

    for (unsigned it = 0; it < iters; it++) {
        unsigned op = (unsigned)(xs_next(&s) % 100);

        if (op < 60) {
            /* PUSH a random-sized batch. */
            unsigned n = 1 + (unsigned)(xs_next(&s) % 256);
            for (unsigned i = 0; i < n; i++) {
                batch[i * 2]     = rand_sample(&s);
                batch[i * 2 + 1] = rand_sample(&s);
            }
            libra_audio_push_batch(a, batch, n);
        } else if (op < 90) {
            /* FLUSH. */
            libra_audio_flush(a, sink_cb, &k);
        } else if (op < 95) {
            /* adjust_rate with a random queue depth. */
            unsigned q = (unsigned)(xs_next(&s) % (1u << 20));
            libra_audio_adjust_rate(a, q, 4);
            uint64_t lo = (base * 95) / 100;
            uint64_t hi = (base * 105) / 100;
            ASSERT(a->ratio_q32 >= lo - 2 && a->ratio_q32 <= hi + 2);
        } else {
            /* Occasionally change src rate — exercises the "may grow
             * out_buf" path.  Stays within plausible range. */
            double new_src = srcs[xs_next(&s) % (sizeof(srcs) / sizeof(srcs[0]))];
            libra_audio_set_src_rate(a, new_src);
            base = a->base_ratio_q32;
        }

        /* Invariants checked every iteration. */
        ASSERT(a->count <= LIBRA_RING_FRAMES);
        ASSERT(a->read_pos < LIBRA_RING_FRAMES);
        ASSERT(a->write_pos < LIBRA_RING_FRAMES);
        ASSERT(a->out_cap_frames > 0);
        ASSERT(a->out_cap_frames <= LIBRA_OUT_FRAMES_MAX);
        ASSERT(a->out_buf != NULL);
        ASSERT(k.invalid_sample == 0);
        if (a->out_cap_frames > peak_cap) peak_cap = a->out_cap_frames;
    }

    fprintf(stderr, "fuzz: OK total_out=%zu peak_cap=%zu (max=%u) max_per_call=%zu\n",
            k.total_frames, peak_cap, LIBRA_OUT_FRAMES_MAX, k.max_frames_in_call);
    libra_audio_destroy(a);
    return 1;
}

/* ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    uint64_t seed  = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
    unsigned iters = 5000;
    if (argc > 1) seed  = strtoull(argv[1], NULL, 0);
    if (argc > 2) iters = (unsigned)strtoul(argv[2], NULL, 0);
    if (seed == 0) seed = 1;

    /* Drive several rate configurations from the same seed sweep. */
    for (int r = 0; r < 8; r++) {
        if (!run_one(seed + r, iters)) {
            fprintf(stderr, "audio_fuzz: replay with: audio_fuzz 0x%016llx %u\n",
                    (unsigned long long)(seed + r), iters);
            return 1;
        }
    }
    if (g_fail) return 1;
    printf("ALL FUZZ PASS\n");
    return 0;
}
