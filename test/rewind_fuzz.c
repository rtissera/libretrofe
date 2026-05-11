// SPDX-License-Identifier: MIT
/*
 * Randomized soak test for the rewind ring.
 *
 * Maintains a shadow stack of ground-truth states and a sequence of random
 * operations (push/pop/reset).  After each pop, compares the popped state
 * to the top of the shadow stack — must match exactly.
 *
 * Reproducible: a single uint64_t seed drives every random choice.  On
 * failure the seed is printed so the run can be replayed.
 *
 * Usage:
 *   rewind_fuzz                 # default: seed=time, 10000 iterations
 *   rewind_fuzz <seed>          # fixed seed
 *   rewind_fuzz <seed> <iters>  # fixed seed + iteration count
 */
#include "rewind.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t xs_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

/* Shadow stack of pushed states — the ground truth.  We track all pushed
 * states even though the ring evicts old ones, then trim the stack to
 * match the ring's actual count whenever we observe an eviction (count
 * after push < expected). */
struct shadow {
    uint8_t **states;
    unsigned  count;
    unsigned  capacity;
    size_t    state_size;
};

static void shadow_init(struct shadow *sh, size_t state_size)
{
    sh->states = NULL;
    sh->count = 0;
    sh->capacity = 0;
    sh->state_size = state_size;
}

static void shadow_push(struct shadow *sh, const uint8_t *state)
{
    if (sh->count == sh->capacity) {
        sh->capacity = sh->capacity ? sh->capacity * 2 : 16;
        sh->states = realloc(sh->states,
                             sh->capacity * sizeof(uint8_t *));
    }
    sh->states[sh->count] = malloc(sh->state_size);
    memcpy(sh->states[sh->count], state, sh->state_size);
    sh->count++;
}

static void shadow_pop(struct shadow *sh)
{
    if (sh->count == 0) return;
    free(sh->states[--sh->count]);
}

static void shadow_clear(struct shadow *sh)
{
    while (sh->count) shadow_pop(sh);
}

/* Drop the OLDEST n entries — matches the ring's tail-eviction behavior. */
static void shadow_evict_oldest(struct shadow *sh, unsigned n)
{
    if (n > sh->count) n = sh->count;
    for (unsigned i = 0; i < n; i++) free(sh->states[i]);
    memmove(sh->states, sh->states + n,
            (sh->count - n) * sizeof(uint8_t *));
    sh->count -= n;
}

static void shadow_destroy(struct shadow *sh)
{
    shadow_clear(sh);
    free(sh->states);
}

/* ---------------------------------------------------------------------- */

static int run(uint64_t seed, unsigned iters)
{
    fprintf(stderr, "fuzz: seed=0x%016llx iters=%u\n",
            (unsigned long long)seed, iters);

    /* Pick test-rig sizes from the seed so each run exercises a different
     * configuration.  Keep state_size moderate so iterations are fast. */
    uint64_t s = seed;
    size_t   state_size = 16 + (xs_next(&s) % 4080);          /* 16..4095 */
    unsigned max_slots  = 2 + (unsigned)(xs_next(&s) % 30);   /* 2..31    */
    /* Ensure max_bytes can hold at least one base AND one worst-case delta. */
    size_t   min_bytes  = state_size * 2 + 64;
    size_t   max_bytes  = min_bytes + (xs_next(&s) % (state_size * max_slots));

    fprintf(stderr, "fuzz: state_size=%zu max_slots=%u max_bytes=%zu\n",
            state_size, max_slots, max_bytes);

    libra_rewind_t *rw = libra_rewind_create(max_slots, max_bytes, state_size);
    if (!rw) { fprintf(stderr, "create failed\n"); return 1; }

    struct shadow sh;
    shadow_init(&sh, state_size);

    uint8_t *current = calloc(1, state_size);  /* "core" state we mutate */

    for (unsigned it = 0; it < iters; it++) {
        unsigned op = (unsigned)(xs_next(&s) % 100);
        unsigned ring_count = libra_rewind_count(rw);

        if (op < 60) {
            /* PUSH — mutate current then push it. */
            unsigned pct = (unsigned)(xs_next(&s) % 30) + 1;
            for (unsigned m = 0; m < pct; m++) {
                size_t pos = (size_t)(xs_next(&s) % state_size);
                current[pos] = (uint8_t)xs_next(&s);
            }
            void *buf = libra_rewind_serialize_buf(rw);
            memcpy(buf, current, state_size);
            if (!libra_rewind_push(rw)) {
                fprintf(stderr, "iter=%u: push failed\n", it);
                goto fail;
            }
            shadow_push(&sh, current);
            /* Reconcile shadow with ring after eviction. */
            unsigned new_count = libra_rewind_count(rw);
            if (new_count < sh.count)
                shadow_evict_oldest(&sh, sh.count - new_count);
        } else if (op < 90) {
            /* POP — rewinds one frame.  Only test when ring has ≥2 slots
             * so the predecessor-of-newest is still a state we have in
             * the shadow stack.  The "pop into the base" edge case (when
             * ring count is 1) is covered by unit tests. */
            if (ring_count < 2) continue;
            size_t got = libra_rewind_pop(rw);
            if (got != state_size) {
                fprintf(stderr, "iter=%u: pop returned %zu\n", it, got);
                goto fail;
            }
            void *out = libra_rewind_serialize_buf(rw);
            /* Pop yields predecessor-of-newest = second-from-top of shadow. */
            const uint8_t *gold = sh.states[sh.count - 2];
            if (memcmp(out, gold, state_size) != 0) {
                fprintf(stderr, "iter=%u: pop mismatch (count was %u)\n",
                        it, ring_count);
                for (size_t i = 0; i < state_size; i++) {
                    if (((uint8_t *)out)[i] != gold[i]) {
                        fprintf(stderr,
                                "  byte %zu got=0x%02x want=0x%02x\n",
                                i, ((uint8_t *)out)[i], gold[i]);
                        break;
                    }
                }
                goto fail;
            }
            /* The popped state becomes the "current" core state for the
             * next push, mirroring how a real frontend would behave. */
            memcpy(current, gold, state_size);
            shadow_pop(&sh);    /* discard the now-undone newest entry */
        } else {
            /* RESET — rare. */
            libra_rewind_reset(rw);
            shadow_clear(&sh);
        }
    }

    shadow_destroy(&sh);
    free(current);
    libra_rewind_destroy(rw);
    fprintf(stderr, "fuzz: OK seed=0x%016llx\n",
            (unsigned long long)seed);
    return 0;

fail:
    fprintf(stderr, "fuzz: FAILED — replay with: rewind_fuzz 0x%016llx %u\n",
            (unsigned long long)seed, iters);
    shadow_destroy(&sh);
    free(current);
    libra_rewind_destroy(rw);
    return 1;
}

int main(int argc, char **argv)
{
    uint64_t seed  = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
    unsigned iters = 10000;
    if (argc > 1) seed  = strtoull(argv[1], NULL, 0);
    if (argc > 2) iters = (unsigned)strtoul(argv[2], NULL, 0);
    if (seed == 0) seed = 1; /* xorshift fixed point */
    return run(seed, iters);
}
