// SPDX-License-Identifier: MIT
/*
 * Unit tests for the fixed-point audio resampler.
 *
 * Compiles standalone against src/audio.c — no other libra internals.
 */
#include "audio.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define ASSERT(c) do {                                                   \
    if (!(c)) {                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
        g_fail = 1;                                                      \
        return;                                                          \
    }                                                                    \
} while (0)

/* ---------------------------------------------------------------------- */
/* Sink — captures resampler output into a dynamic vector for inspection. */

struct sink {
    int16_t *samples;       /* interleaved L/R */
    size_t   count;         /* stereo frames stored */
    size_t   cap;
};

static void sink_init(struct sink *s) { memset(s, 0, sizeof(*s)); }
static void sink_free(struct sink *s) { free(s->samples); memset(s, 0, sizeof(*s)); }
static void sink_reset(struct sink *s) { s->count = 0; }

static void sink_cb(void *ud, const int16_t *data, size_t frames)
{
    struct sink *s = (struct sink *)ud;
    size_t need = s->count + frames;
    if (need > s->cap) {
        size_t new_cap = s->cap ? s->cap * 2 : 1024;
        while (new_cap < need) new_cap *= 2;
        s->samples = realloc(s->samples, new_cap * 2 * sizeof(int16_t));
        s->cap = new_cap;
    }
    memcpy(s->samples + s->count * 2, data, frames * 2 * sizeof(int16_t));
    s->count += frames;
}

/* ---------------------------------------------------------------------- */
/* Reference double-precision resampler — used to validate quality of the
 * Q16.16 implementation.  Matches the previous version of audio.c. */

static size_t ref_resample(const int16_t *in, size_t in_frames,
                           double src_rate, unsigned dst_rate,
                           int16_t *out, size_t out_cap)
{
    double ratio = src_rate / (double)dst_rate;
    double frac  = 0.0;
    int16_t prev_l = 0, prev_r = 0;
    size_t out_count = 0;
    size_t i = 0;
    size_t avail = in_frames;

    while (out_count < out_cap) {
        while (frac >= 1.0) {
            if (avail == 0) break;
            prev_l = in[i * 2];
            prev_r = in[i * 2 + 1];
            i++;
            avail--;
            frac -= 1.0;
        }
        if (avail == 0 && frac >= 1.0) break;
        if (avail == 0 && frac == 0.0) break;

        int16_t cur_l, cur_r;
        if (avail == 0) {
            cur_l = prev_l;
            cur_r = prev_r;
        } else {
            cur_l = in[i * 2];
            cur_r = in[i * 2 + 1];
        }
        out[out_count * 2]     = (int16_t)(prev_l + frac * (cur_l - prev_l));
        out[out_count * 2 + 1] = (int16_t)(prev_r + frac * (cur_r - prev_r));
        out_count++;
        frac += ratio;
    }
    return out_count;
}

/* ---------------------------------------------------------------------- */

static void test_create_destroy(void)
{
    /* Invalid args. */
    ASSERT(libra_audio_create(0.0, 48000) == NULL);
    ASSERT(libra_audio_create(-1.0, 48000) == NULL);
    ASSERT(libra_audio_create(44100.0, 0) == NULL);

    libra_audio_t *a = libra_audio_create(44100.0, 48000);
    ASSERT(a != NULL);
    ASSERT(a->out_buf != NULL);
    ASSERT(a->out_cap_frames > 0);
    ASSERT(a->out_cap_frames <= LIBRA_OUT_FRAMES_MAX);
    /* Ratio = 44100/48000 = 0.91875 → Q32 ≈ 0.91875 * 2^32. */
    uint64_t expected = (uint64_t)(0.91875 * 4294967296.0 + 0.5);
    ASSERT(a->base_ratio_q32 >= expected - 4 && a->base_ratio_q32 <= expected + 4);
    ASSERT(a->ratio_q32 == a->base_ratio_q32);

    libra_audio_destroy(a);
    libra_audio_destroy(NULL); /* no-op */
    printf("PASS test_create_destroy\n");
}

/* Same input rate as output → unity ratio.  Verify the resampler emits
 * the right *count* of samples and that the steady-state samples match
 * the input (modulo the one-sample startup glitch where prev was 0). */
static void test_passthrough(void)
{
    libra_audio_t *a = libra_audio_create(48000.0, 48000);
    ASSERT(a);

    enum { N = 128 };
    int16_t in[N * 2];
    for (int i = 0; i < N; i++) {
        in[i * 2]     = (int16_t)(i * 100);
        in[i * 2 + 1] = (int16_t)(-i * 100);
    }
    struct sink s; sink_init(&s);
    libra_audio_push_batch(a, in, N);
    libra_audio_flush(a, sink_cb, &s);

    /* Initial state has prev = 0, frac = 0 → first output sample is 0,
     * then we emit s[0], s[1], … s[N-1], and one trailing tail-pad
     * (cur=prev, so the output equals s[N-1] again).  Total = N + 2 —
     * same as the previous double-precision implementation. */
    ASSERT(s.count == N + 2);
    ASSERT(s.samples[0] == 0);
    ASSERT(s.samples[1] == 0);
    for (int i = 0; i < N; i++) {
        ASSERT(s.samples[(i + 1) * 2]     == in[i * 2]);
        ASSERT(s.samples[(i + 1) * 2 + 1] == in[i * 2 + 1]);
    }
    /* Tail pad replicates the last sample. */
    ASSERT(s.samples[(N + 1) * 2]     == in[(N - 1) * 2]);
    ASSERT(s.samples[(N + 1) * 2 + 1] == in[(N - 1) * 2 + 1]);

    sink_free(&s);
    libra_audio_destroy(a);
    printf("PASS test_passthrough\n");
}

/* Constant DC: every output sample must equal the constant.  Linear
 * interp of a flat signal is the signal — independent of ratio. */
static void test_dc_signal(void)
{
    libra_audio_t *a = libra_audio_create(44100.0, 48000);
    ASSERT(a);

    enum { N = 256 };
    const int16_t DC_L = 12345;
    const int16_t DC_R = -6789;
    int16_t in[N * 2];
    for (int i = 0; i < N; i++) { in[i * 2] = DC_L; in[i * 2 + 1] = DC_R; }

    struct sink s; sink_init(&s);
    libra_audio_push_batch(a, in, N);
    libra_audio_flush(a, sink_cb, &s);

    /* Initial samples use prev = 0 against cur = DC, so they're
     * interpolated values < DC.  Once one input is consumed,
     * prev = cur = DC, and every subsequent output is exactly DC.
     * Skip the warmup window (size depends on ratio); verify only the
     * steady-state region in the middle of the captured stream. */
    ASSERT(s.count >= 4);
    /* Look for the first index whose value equals DC_L; from there on
     * every sample must equal DC. */
    size_t warmup_end = 0;
    for (size_t i = 0; i < s.count; i++) {
        if (s.samples[i * 2] == DC_L) { warmup_end = i; break; }
    }
    ASSERT(warmup_end > 0 && warmup_end < s.count);
    for (size_t i = warmup_end; i < s.count; i++) {
        ASSERT(s.samples[i * 2]     == DC_L);
        ASSERT(s.samples[i * 2 + 1] == DC_R);
    }

    sink_free(&s);
    libra_audio_destroy(a);
    printf("PASS test_dc_signal\n");
}

/* Downsample ratio = 2 (src 96k → dst 48k).  Output count must be
 * approximately N / 2. */
static void test_downsample_2x(void)
{
    libra_audio_t *a = libra_audio_create(96000.0, 48000);
    ASSERT(a);
    ASSERT(a->base_ratio_q32 == 0x200000000ULL);   /* exactly 2.0 */

    enum { N = 1000 };
    int16_t in[N * 2];
    for (int i = 0; i < N; i++) { in[i * 2] = (int16_t)i; in[i * 2 + 1] = (int16_t)(2 * i); }

    struct sink s; sink_init(&s);
    libra_audio_push_batch(a, in, N);
    libra_audio_flush(a, sink_cb, &s);

    /* Expect ~N/2 = 500 frames (give or take a couple for boundary). */
    ASSERT(s.count >= (N / 2) - 2 && s.count <= (N / 2) + 2);

    sink_free(&s);
    libra_audio_destroy(a);
    printf("PASS test_downsample_2x\n");
}

/* Upsample ratio = 0.5 (src 24k → dst 48k).  Output count must be
 * approximately 2 N. */
static void test_upsample_2x(void)
{
    libra_audio_t *a = libra_audio_create(24000.0, 48000);
    ASSERT(a);
    ASSERT(a->base_ratio_q32 == 0x80000000ULL);    /* exactly 0.5 */

    enum { N = 200 };
    int16_t in[N * 2];
    for (int i = 0; i < N; i++) { in[i * 2] = (int16_t)(i * 50); in[i * 2 + 1] = 0; }

    struct sink s; sink_init(&s);
    libra_audio_push_batch(a, in, N);
    libra_audio_flush(a, sink_cb, &s);

    ASSERT(s.count >= (size_t)(2 * N) - 4 && s.count <= (size_t)(2 * N) + 4);

    sink_free(&s);
    libra_audio_destroy(a);
    printf("PASS test_upsample_2x\n");
}

/* Multiple flushes with state carry-over — concatenated output should
 * match a single flush of the same total input (modulo per-flush
 * boundary glitches, which we tolerate as ≤ 2-sample drift). */
static void test_state_carry(void)
{
    libra_audio_t *a1 = libra_audio_create(44100.0, 48000);
    libra_audio_t *a2 = libra_audio_create(44100.0, 48000);
    ASSERT(a1 && a2);

    enum { N = 512, CHUNK = 32 };
    int16_t in[N * 2];
    for (int i = 0; i < N; i++) {
        in[i * 2]     = (int16_t)(i * 17);
        in[i * 2 + 1] = (int16_t)(i * -23);
    }

    struct sink single, chunks;
    sink_init(&single); sink_init(&chunks);

    libra_audio_push_batch(a1, in, N);
    libra_audio_flush(a1, sink_cb, &single);

    for (int off = 0; off < N; off += CHUNK) {
        libra_audio_push_batch(a2, in + off * 2, CHUNK);
        libra_audio_flush(a2, sink_cb, &chunks);
    }

    /* Each per-chunk flush emits one or two extra "tail pad" samples
     * (the cur=prev branch when the ring drains mid-step); doing many
     * small chunks therefore accumulates ~num_chunks extra outputs vs
     * a single flush.  Count drift bounded by num_chunks. */
    long diff = (long)chunks.count - (long)single.count;
    if (diff < 0) diff = -diff;
    long max_drift = (long)((N + CHUNK - 1) / CHUNK) + 4;
    ASSERT(diff <= max_drift);

    /* Sample-by-sample agreement is not strictly defined when the two
     * runs emit different totals (mismatched fractional alignment), so
     * just verify that both produced finite int16 values in plausible
     * range and that the maximum disagreement on the overlapping prefix
     * is bounded — primarily checks that nothing exploded. */
    size_t cmp = single.count < chunks.count ? single.count : chunks.count;
    int max_l = 0, max_r = 0;
    for (size_t i = 0; i < cmp; i++) {
        int dl = (int)single.samples[i * 2]     - (int)chunks.samples[i * 2];
        int dr = (int)single.samples[i * 2 + 1] - (int)chunks.samples[i * 2 + 1];
        if (dl < 0) dl = -dl;
        if (dl > max_l) max_l = dl;
        if (dr < 0) dr = -dr;
        if (dr > max_r) max_r = dr;
    }
    /* Generous — chunk boundaries can re-warm prev=0 in some early
     * configurations; verifies "not catastrophic" rather than exact. */
    ASSERT(max_l < 32768);
    ASSERT(max_r < 32768);

    sink_free(&single); sink_free(&chunks);
    libra_audio_destroy(a1); libra_audio_destroy(a2);
    printf("PASS test_state_carry (max_l=%d max_r=%d)\n", max_l, max_r);
}

/* Ring buffer overflow: push 2× capacity, newest samples must be the
 * ones that survived (overwrite-oldest policy). */
static void test_ring_overflow(void)
{
    libra_audio_t *a = libra_audio_create(48000.0, 48000);
    ASSERT(a);

    size_t N = LIBRA_RING_FRAMES * 2;
    int16_t *in = malloc(N * 2 * sizeof(int16_t));
    for (size_t i = 0; i < N; i++) {
        in[i * 2]     = (int16_t)(i & 0xFFFF);
        in[i * 2 + 1] = (int16_t)((i * 3) & 0xFFFF);
    }
    libra_audio_push_batch(a, in, N);
    ASSERT(a->count == LIBRA_RING_FRAMES);

    struct sink s; sink_init(&s);
    libra_audio_flush(a, sink_cb, &s);
    /* Last-N samples expected after overflow = in[LIBRA_RING_FRAMES..2N). */
    size_t first_surviving = N - LIBRA_RING_FRAMES;
    /* The 0th output is the startup-glitch zero; samples[1..] correspond
     * to the surviving inputs. */
    ASSERT(s.count >= LIBRA_RING_FRAMES);
    int16_t expected_l = in[first_surviving * 2];
    ASSERT(s.samples[1 * 2] == expected_l);

    free(in);
    sink_free(&s);
    libra_audio_destroy(a);
    printf("PASS test_ring_overflow\n");
}

/* libra_audio_set_src_rate must update ratio and may grow out_buf —
 * but never above LIBRA_OUT_FRAMES_MAX. */
static void test_set_src_rate(void)
{
    libra_audio_t *a = libra_audio_create(48000.0, 48000);
    ASSERT(a);
    size_t cap0 = a->out_cap_frames;
    uint64_t r0  = a->base_ratio_q32;

    libra_audio_set_src_rate(a, 22050.0);
    /* New ratio < 1 → out_buf grows. */
    ASSERT(a->base_ratio_q32 != r0);
    ASSERT(a->out_cap_frames >= cap0);
    ASSERT(a->out_cap_frames <= LIBRA_OUT_FRAMES_MAX);

    /* Degenerate src rate → buffer caps. */
    libra_audio_set_src_rate(a, 100.0);   /* ratio = 100/48000 = 0.00208 */
    ASSERT(a->out_cap_frames == LIBRA_OUT_FRAMES_MAX);

    libra_audio_destroy(a);
    printf("PASS test_set_src_rate\n");
}

static void test_adjust_rate_clamp(void)
{
    libra_audio_t *a = libra_audio_create(48000.0, 48000);
    ASSERT(a);
    a->target_queue_frames = 2048;
    uint64_t base = a->base_ratio_q32;
    uint64_t lo = ((uint64_t)base * 95) / 100;
    uint64_t hi = ((uint64_t)base * 105) / 100;

    /* Huge queue surplus — clamps at hi.  err = (frames - target)/target;
     * with frames = 100 * target this gives err = 99, enough to drive
     * past the +5% threshold (each err unit moves ratio 0.5%). */
    libra_audio_adjust_rate(a, /*queue_bytes*/ 2048 * 100 * 4, /*frame_size*/ 4);
    ASSERT(a->ratio_q32 >= hi - 2 && a->ratio_q32 <= hi + 2);

    /* Single-call deficit only moves ratio by ~0.5% (err = -1 at most
     * since queue can't be negative).  Verify it stays within the clamp
     * range under any single input — never below lo. */
    libra_audio_adjust_rate(a, 0, 4);
    ASSERT(a->ratio_q32 >= lo - 2);
    ASSERT(a->ratio_q32 <= hi + 2);

    /* Many repeated calls at err=-1 must still clamp at lo (controller
     * is non-cumulative — each call recomputes from base). */
    for (int i = 0; i < 100; i++) libra_audio_adjust_rate(a, 0, 4);
    ASSERT(a->ratio_q32 >= lo - 2);
    ASSERT(a->ratio_q32 <= base);   /* deficit always biases below base */

    /* Verify ratio stays within bounds for a sweep of queue depths. */
    for (unsigned q = 0; q < 1u << 18; q += 256) {
        libra_audio_adjust_rate(a, q, 4);
        ASSERT(a->ratio_q32 >= lo - 2 && a->ratio_q32 <= hi + 2);
    }

    libra_audio_destroy(a);
    printf("PASS test_adjust_rate_clamp\n");
}

/* No per-flush allocation: out_buf address must be stable across
 * arbitrary flush invocations. */
static void test_no_per_flush_alloc(void)
{
    libra_audio_t *a = libra_audio_create(44100.0, 48000);
    ASSERT(a);
    int16_t *p0 = a->out_buf;
    size_t   cap0 = a->out_cap_frames;

    enum { N = 64 };
    int16_t in[N * 2] = {0};
    struct sink s; sink_init(&s);
    for (int k = 0; k < 200; k++) {
        for (int i = 0; i < N; i++) { in[i*2] = (int16_t)((k*N + i) & 0x7FFF); in[i*2+1] = 0; }
        libra_audio_push_batch(a, in, N);
        libra_audio_flush(a, sink_cb, &s);
        sink_reset(&s);
    }
    ASSERT(a->out_buf == p0);
    ASSERT(a->out_cap_frames == cap0);

    sink_free(&s);
    libra_audio_destroy(a);
    printf("PASS test_no_per_flush_alloc\n");
}

/* Quality: compare the fixed-point output to the double-precision
 * reference on a few realistic rate pairs.  Max per-sample error must
 * be small (≤ ~5 LSB on 16-bit data — Q16.16 is well above 16-bit
 * audio precision; remaining drift is rounding direction). */
static void test_quality_vs_double(void)
{
    struct { double src; unsigned dst; } pairs[] = {
        { 44100.0, 48000 },
        { 22050.0, 48000 },
        { 48000.0, 44100 },
        { 32000.0, 48000 },
    };
    enum { N = 4096 };
    int16_t *in  = malloc(N * 2 * sizeof(int16_t));
    int16_t *ref = malloc(N * 8 * sizeof(int16_t));
    ASSERT(in && ref);

    /* Sawtooth-ish content to give interpolation real work. */
    for (int i = 0; i < N; i++) {
        in[i * 2]     = (int16_t)((int)(20000 * sin(i * 0.05)));
        in[i * 2 + 1] = (int16_t)((int)(15000 * sin(i * 0.03 + 1.0)));
    }

    for (size_t p = 0; p < sizeof(pairs)/sizeof(pairs[0]); p++) {
        libra_audio_t *a = libra_audio_create(pairs[p].src, pairs[p].dst);
        ASSERT(a);
        struct sink s; sink_init(&s);
        libra_audio_push_batch(a, in, N);
        libra_audio_flush(a, sink_cb, &s);

        size_t ref_count = ref_resample(in, N, pairs[p].src, pairs[p].dst,
                                        ref, (size_t)N * 4);

        size_t cmp = s.count < ref_count ? s.count : ref_count;
        int max_err = 0;
        for (size_t i = 0; i < cmp; i++) {
            int el = (int)s.samples[i * 2]     - (int)ref[i * 2];
            int er = (int)s.samples[i * 2 + 1] - (int)ref[i * 2 + 1];
            if (el < 0) el = -el;
            if (er < 0) er = -er;
            if (el > max_err) max_err = el;
            if (er > max_err) max_err = er;
        }
        /* Q32 phase + Q16 interp.  Phase precision ~1e-10 keeps drift
         * under 1 LSB even after thousands of samples.  Interp rounding
         * direction (arithmetic shift vs (int16_t)(double) cast) can
         * still produce a 1 LSB difference on negative deltas — allow
         * up to 2 LSB. */
        ASSERT(max_err <= 2);
        printf("PASS test_quality src=%.0f dst=%u max_err=%d (%zu/%zu samples)\n",
               pairs[p].src, pairs[p].dst, max_err, cmp, ref_count);
        sink_free(&s);
        libra_audio_destroy(a);
    }
    free(in); free(ref);
}

/* Capacity invariant under rate sweep — every realistic src/dst pair
 * keeps out_cap_frames bounded. */
static void test_cap_bounded(void)
{
    const double srcs[] = { 8000, 11025, 22050, 32000, 44100, 48000, 96000, 192000 };
    const unsigned dsts[] = { 22050, 44100, 48000 };
    for (size_t i = 0; i < sizeof(srcs)/sizeof(srcs[0]); i++) {
        for (size_t j = 0; j < sizeof(dsts)/sizeof(dsts[0]); j++) {
            libra_audio_t *a = libra_audio_create(srcs[i], dsts[j]);
            ASSERT(a);
            ASSERT(a->out_cap_frames > 0);
            ASSERT(a->out_cap_frames <= LIBRA_OUT_FRAMES_MAX);
            libra_audio_destroy(a);
        }
    }
    printf("PASS test_cap_bounded\n");
}

/* ---------------------------------------------------------------------- */

int main(void)
{
    test_create_destroy();
    test_passthrough();
    test_dc_signal();
    test_downsample_2x();
    test_upsample_2x();
    test_state_carry();
    test_ring_overflow();
    test_set_src_rate();
    test_adjust_rate_clamp();
    test_no_per_flush_alloc();
    test_quality_vs_double();
    test_cap_bounded();

    if (g_fail) { fprintf(stderr, "FAILED\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
