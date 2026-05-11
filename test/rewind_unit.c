// SPDX-License-Identifier: MIT
/*
 * Deterministic unit tests for the rewind ring.
 * Builds without depending on the rest of libra — rewind.c is compiled
 * directly into this binary.
 */
#include "rewind.h"

#include <assert.h>
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

/* xorshift64 — deterministic so failures reproduce. */
static uint64_t xs_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static void fill_random(uint8_t *buf, size_t n, uint64_t *seed)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v = xs_next(seed);
        memcpy(buf + i, &v, 8);
    }
    if (i < n) {
        uint64_t v = xs_next(seed);
        memcpy(buf + i, &v, n - i);
    }
}

/* Mutate ~pct percent of bytes, scattered. */
static void mutate(uint8_t *buf, size_t n, unsigned pct, uint64_t *seed)
{
    size_t k = (n * pct) / 100;
    if (k == 0) k = 1;
    for (size_t i = 0; i < k; i++) {
        size_t pos = (size_t)(xs_next(seed) % n);
        buf[pos] = (uint8_t)xs_next(seed);
    }
}

/* ---------------------------------------------------------------------- */

static void test_create_invalid(void)
{
    ASSERT(!libra_rewind_create(0, 1024, 64));
    ASSERT(!libra_rewind_create(4, 0, 64));
    ASSERT(!libra_rewind_create(4, 1024, 0));
    /* max_bytes too small for a base slot */
    ASSERT(!libra_rewind_create(4, 4, 1024));
    /* Valid */
    libra_rewind_t *rw = libra_rewind_create(4, 4096, 64);
    ASSERT(rw);
    ASSERT(libra_rewind_state_size(rw) == 64);
    ASSERT(libra_rewind_count(rw) == 0);
    libra_rewind_destroy(rw);
    printf("PASS test_create_invalid\n");
}

static void test_pop_empty(void)
{
    libra_rewind_t *rw = libra_rewind_create(4, 4096, 64);
    ASSERT(rw);
    ASSERT(libra_rewind_pop(rw) == 0);
    libra_rewind_destroy(rw);
    printf("PASS test_pop_empty\n");
}

static void test_roundtrip_one(size_t state_size)
{
    libra_rewind_t *rw = libra_rewind_create(4, state_size * 4 + 256,
                                              state_size);
    ASSERT(rw);

    uint8_t *gold = malloc(state_size);
    uint64_t seed = 0xCAFEBABEull ^ state_size;
    fill_random(gold, state_size, &seed);

    void *buf = libra_rewind_serialize_buf(rw);
    memcpy(buf, gold, state_size);
    ASSERT(libra_rewind_push(rw));
    ASSERT(libra_rewind_count(rw) == 1);

    /* Scribble cur (it flipped after push) to make sure pop actually
     * restores from the ring rather than from leftover memory. */
    void *scribble = libra_rewind_serialize_buf(rw);
    memset(scribble, 0xAA, state_size);

    ASSERT(libra_rewind_pop(rw) == state_size);
    void *out = libra_rewind_serialize_buf(rw);
    ASSERT(memcmp(out, gold, state_size) == 0);
    ASSERT(libra_rewind_count(rw) == 0);

    free(gold);
    libra_rewind_destroy(rw);
    printf("PASS test_roundtrip_one(%zu)\n", state_size);
}

/* Pop semantics: each pop rewinds ONE frame.  After K pops on a ring with
 * N pushes, the core state should be gold[N-1-K] for K in [1..N-1], and
 * gold[0] for K==N (the base slot re-asserts the oldest state). */
static unsigned expected_idx_after_pops(unsigned n, unsigned popped)
{
    if (popped >= n) return 0;
    return n - 1 - popped;
}

static void test_multi_pop(size_t state_size, unsigned n, unsigned change_pct)
{
    libra_rewind_t *rw = libra_rewind_create(
        n + 4, state_size * (n + 4) + 1024, state_size);
    ASSERT(rw);

    uint8_t **gold = malloc(n * sizeof(uint8_t *));
    uint64_t seed = 0xDEADBEEFull ^ state_size ^ n ^ change_pct;
    for (unsigned k = 0; k < n; k++) {
        gold[k] = malloc(state_size);
        if (k == 0)
            fill_random(gold[k], state_size, &seed);
        else {
            memcpy(gold[k], gold[k - 1], state_size);
            mutate(gold[k], state_size, change_pct, &seed);
        }
        void *buf = libra_rewind_serialize_buf(rw);
        memcpy(buf, gold[k], state_size);
        ASSERT(libra_rewind_push(rw));
    }
    ASSERT(libra_rewind_count(rw) == n);

    for (unsigned popped = 1; popped <= n; popped++) {
        ASSERT(libra_rewind_pop(rw) == state_size);
        unsigned exp = expected_idx_after_pops(n, popped);
        void *out = libra_rewind_serialize_buf(rw);
        if (memcmp(out, gold[exp], state_size) != 0) {
            const uint8_t *a = out, *b = gold[exp];
            for (size_t i = 0; i < state_size; i++)
                if (a[i] != b[i]) {
                    fprintf(stderr,
                            "  popped=%u exp=gold[%u] byte %zu got=0x%02x want=0x%02x\n",
                            popped, exp, i, a[i], b[i]);
                    break;
                }
            ASSERT(0);
        }
    }
    ASSERT(libra_rewind_count(rw) == 0);

    for (unsigned k = 0; k < n; k++) free(gold[k]);
    free(gold);
    libra_rewind_destroy(rw);
    printf("PASS test_multi_pop(state=%zu n=%u change=%u%%)\n",
           state_size, n, change_pct);
}

static void test_eviction_by_slots(void)
{
    const size_t   S = 256;
    const unsigned MAX_SLOTS = 4;

    libra_rewind_t *rw = libra_rewind_create(MAX_SLOTS, S * 16, S);
    ASSERT(rw);

    uint8_t **gold = malloc(10 * sizeof(uint8_t *));
    uint64_t seed = 0xA5A5A5A5ull;
    for (unsigned k = 0; k < 10; k++) {
        gold[k] = malloc(S);
        if (k == 0) fill_random(gold[k], S, &seed);
        else { memcpy(gold[k], gold[k-1], S); mutate(gold[k], S, 5, &seed); }
        void *buf = libra_rewind_serialize_buf(rw);
        memcpy(buf, gold[k], S);
        ASSERT(libra_rewind_push(rw));
    }

    /* Exactly MAX_SLOTS retained — newest 4 of 10 pushes. */
    ASSERT(libra_rewind_count(rw) == MAX_SLOTS);

    /* All 4 surviving slots are deltas (the base for gold[0] was evicted
     * long ago).  Popping them yields gold[8], gold[7], gold[6], gold[5]. */
    for (unsigned popped = 1; popped <= MAX_SLOTS; popped++) {
        ASSERT(libra_rewind_pop(rw) == S);
        unsigned exp = 9 - popped;   /* 8, 7, 6, 5 */
        void *out = libra_rewind_serialize_buf(rw);
        ASSERT(memcmp(out, gold[exp], S) == 0);
    }
    ASSERT(libra_rewind_count(rw) == 0);

    for (unsigned k = 0; k < 10; k++) free(gold[k]);
    free(gold);
    libra_rewind_destroy(rw);
    printf("PASS test_eviction_by_slots\n");
}

static void test_eviction_by_bytes(void)
{
    /* Tight ring forces byte-overlap eviction before slot count fills. */
    const size_t S = 1024;
    libra_rewind_t *rw = libra_rewind_create(64, S * 3, S);
    ASSERT(rw);

    uint8_t **gold = malloc(20 * sizeof(uint8_t *));
    uint64_t seed = 0x12345678ull;
    for (unsigned k = 0; k < 20; k++) {
        gold[k] = malloc(S);
        if (k == 0) fill_random(gold[k], S, &seed);
        else { memcpy(gold[k], gold[k-1], S); mutate(gold[k], S, 50, &seed); }
        void *buf = libra_rewind_serialize_buf(rw);
        memcpy(buf, gold[k], S);
        ASSERT(libra_rewind_push(rw));
    }
    /* count is whatever fit — exact value depends on encoded sizes, but
     * must be >= 1 and <= 20.  Pop everything and verify the most-recent
     * states still round-trip. */
    unsigned cnt = libra_rewind_count(rw);
    ASSERT(cnt >= 1 && cnt <= 20);

    /* Pop pulls back through the surviving slots.  If the base survived,
     * the last pop re-asserts gold[20-cnt]; if not, the last pop yields
     * gold[19-cnt].  Test both possibilities by popping cnt-1 deltas (we
     * know those are deltas regardless) and stopping there. */
    for (unsigned popped = 1; popped < cnt; popped++) {
        ASSERT(libra_rewind_pop(rw) == S);
        unsigned exp = 19 - popped;
        void *out = libra_rewind_serialize_buf(rw);
        ASSERT(memcmp(out, gold[exp], S) == 0);
    }

    for (unsigned k = 0; k < 20; k++) free(gold[k]);
    free(gold);
    libra_rewind_destroy(rw);
    printf("PASS test_eviction_by_bytes\n");
}

static void test_reset(void)
{
    const size_t S = 128;
    libra_rewind_t *rw = libra_rewind_create(8, S * 16, S);
    ASSERT(rw);

    uint8_t a[S], b[S];
    uint64_t seed = 0xFEEDFACEull;
    fill_random(a, S, &seed);
    fill_random(b, S, &seed);

    void *buf = libra_rewind_serialize_buf(rw);
    memcpy(buf, a, S);
    ASSERT(libra_rewind_push(rw));

    libra_rewind_reset(rw);
    ASSERT(libra_rewind_count(rw) == 0);
    ASSERT(libra_rewind_pop(rw) == 0);

    buf = libra_rewind_serialize_buf(rw);
    memcpy(buf, b, S);
    ASSERT(libra_rewind_push(rw));

    ASSERT(libra_rewind_pop(rw) == S);
    void *out = libra_rewind_serialize_buf(rw);
    ASSERT(memcmp(out, b, S) == 0);

    libra_rewind_destroy(rw);
    printf("PASS test_reset\n");
}

static void test_branch(void)
{
    /* Push A, B, C; pop to A; push C', D'; pop to A. */
    const size_t S = 256;
    libra_rewind_t *rw = libra_rewind_create(16, S * 32, S);
    ASSERT(rw);

    uint8_t a[S], b[S], c[S], cprime[S], dprime[S];
    uint64_t seed = 0xBADD00D5ull;
    fill_random(a, S, &seed);
    memcpy(b, a, S); mutate(b, S, 10, &seed);
    memcpy(c, b, S); mutate(c, S, 10, &seed);
    memcpy(cprime, a, S); mutate(cprime, S, 5, &seed);
    memcpy(dprime, cprime, S); mutate(dprime, S, 7, &seed);

#define PUSH(state) do { \
    void *p = libra_rewind_serialize_buf(rw); \
    memcpy(p, (state), S); \
    ASSERT(libra_rewind_push(rw)); \
} while (0)
#define POP_EQ(state) do { \
    ASSERT(libra_rewind_pop(rw) == S); \
    void *p = libra_rewind_serialize_buf(rw); \
    ASSERT(memcmp(p, (state), S) == 0); \
} while (0)

    PUSH(a);            /* base                              */
    PUSH(b);            /* delta b vs a                      */
    PUSH(c);            /* delta c vs b                      */
    POP_EQ(b);          /* rewind c → b                      */
    POP_EQ(a);          /* rewind b → a                      */
    POP_EQ(a);          /* base slot re-asserts a            */

    /* Ring is now empty — at gold==a.  Branch off: a different timeline. */
    PUSH(cprime);       /* base again (count was 0)          */
    PUSH(dprime);       /* delta dprime vs cprime            */
    POP_EQ(cprime);     /* rewind dprime → cprime            */
    POP_EQ(cprime);     /* base re-asserts cprime            */

#undef PUSH
#undef POP_EQ
    libra_rewind_destroy(rw);
    printf("PASS test_branch\n");
}

static void test_state_sizes(void)
{
    /* Exercise tail-byte paths (state_size % 4 != 0) and small sizes. */
    const size_t sizes[] = { 1, 3, 4, 7, 16, 17, 100, 1023, 1024, 4099, 65537 };
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        test_roundtrip_one(sizes[i]);
        test_multi_pop(sizes[i], 8, 10);
    }
}

/* ---------------------------------------------------------------------- */

int main(void)
{
    test_create_invalid();
    test_pop_empty();
    test_state_sizes();
    test_multi_pop(16384, 32,  1);   /* sparse changes — memcmp fast path */
    test_multi_pop(16384, 32, 50);   /* dense changes — encoder slow path */
    test_eviction_by_slots();
    test_eviction_by_bytes();
    test_reset();
    test_branch();

    if (g_fail) {
        fprintf(stderr, "FAILED\n");
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
