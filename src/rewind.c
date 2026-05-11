// SPDX-License-Identifier: MIT
#include "rewind.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Sentinel terminating a delta slot's record stream. UINT32_MAX is safe
 * because the encoder breaks out of run-scan as soon as it walks off the
 * end of the state — it never emits a skip that large. */
#define REWIND_SENTINEL UINT32_MAX

/* Block size for the memcmp fast-skip path.  256 bytes = 4 cache lines on
 * common ARM cores.  Large enough that the memcmp setup amortises, small
 * enough that a non-equal block doesn't waste much scalar work. */
#define BLK_WORDS 64

/* Portable 4-wide SIMD: GCC/Clang vector_size lowers to NEON / SSE2 / RVV
 * automatically; on ISAs without packed SIMD (e.g. mips32r2) the compiler
 * emits scalar 32-bit ops, still pipelined.  Aligned to 4 because we feed
 * it through __builtin_memcpy — that idiom compiles to native unaligned
 * loads on every target while staying strict-aliasing-clean. */
typedef uint32_t v_u32 __attribute__((vector_size(16), aligned(4)));

struct libra_rewind {
    /* -----------------------------------------------------------------------
     * Two state buffers, ping-pong on every push.  See push() below for the
     * invariants — the upshot is that no per-push state-sized memcpy is
     * needed (the prior delta_ref design copied state_size bytes/push).
     * -------------------------------------------------------------------- */
    uint8_t   *buf_a;
    uint8_t   *buf_b;
    uint8_t   *cur;       /* exposed by serialize_buf — flips on push */
    uint8_t   *prev;      /* delta reference for next push            */

    size_t     state_size;
    size_t     state_words;   /* state_size / sizeof(uint32_t) */
    size_t     tail_bytes;    /* state_size % sizeof(uint32_t) */

    /* Worst-case encoded slot size.  Used to pre-check buffer space before
     * encoding directly into the ring (no compress_buf scratch). */
    size_t     slot_upper_bound;

    /* Circular byte buffer holding raw or delta-encoded slot payloads.
     * Encoded data is written DIRECTLY here — there is no separate scratch
     * buffer to memcpy through. */
    uint8_t   *buf;
    size_t     buf_size;
    size_t     buf_head;

    /* Slot metadata — parallel arrays indexed by the slot ring. */
    size_t    *slot_offsets;
    size_t    *slot_sizes;
    bool      *slot_is_base;

    unsigned   max_slots;
    unsigned   head;
    unsigned   tail;
    unsigned   count;
};

/* -------------------------------------------------------------------------
 * Delta-RLE encode / decode on uint32 words.
 *
 * Format per delta slot:
 *   record   := u32 skip_words | u32 nchanged | nchanged * u32 xor_payload
 *   slot     := record* sentinel
 *   sentinel := u32 UINT32_MAX
 *   tail     := tail_bytes raw XOR (only when state_size % 4 != 0)
 *
 * Three speedups baked in:
 *   1. __builtin_memcmp on 256-byte blocks fast-skips equal regions.
 *      Compiler inlines this to a tight vector loop on every supported
 *      target.  This is the dominant win for typical sparse-change states.
 *   2. vector_size XOR for payload writes (4 lanes per iteration).
 *   3. encode writes directly into the caller-supplied output buffer (the
 *      ring).  No scratch.  Saves state_size*2+32 bytes of resident memory.
 * ---------------------------------------------------------------------- */

static size_t encode_delta(const uint32_t * restrict src,
                           const uint32_t * restrict ref,
                           uint8_t       * restrict out_buf,
                           size_t                   n_words,
                           const uint8_t * restrict src_tail,
                           const uint8_t * restrict ref_tail,
                           size_t                   tail_bytes)
{
    uint8_t  *out_start = out_buf;
    uint32_t *o         = (uint32_t *)out_buf;
    size_t    i = 0, last_emit = 0;

    while (i < n_words) {
        /* Fast-skip whole equal blocks.  __builtin_memcmp inlines on every
         * supported target; this collapses cost on the hot common case
         * (95-99% of save-state bytes typically don't change frame-to-frame). */
        while (i + BLK_WORDS <= n_words &&
               __builtin_memcmp(&src[i], &ref[i],
                                BLK_WORDS * sizeof(uint32_t)) == 0)
            i += BLK_WORDS;

        /* Word-scan equal until first diff (≤ BLK_WORDS-1 iterations). */
        while (i < n_words && src[i] == ref[i])
            i++;
        if (i == n_words)
            break;

        size_t skip      = i - last_emit;
        size_t chg_start = i;

        /* Find run of differing words. */
        while (i < n_words && src[i] != ref[i])
            i++;
        size_t nchanged = i - chg_start;

        *o++ = (uint32_t)skip;
        *o++ = (uint32_t)nchanged;

        /* XOR payload — 4-lane vector for the bulk, scalar tail. */
        size_t k     = 0;
        size_t v_run = nchanged & ~(size_t)3;
        for (; k < v_run; k += 4) {
            v_u32 a, b, d;
            __builtin_memcpy(&a, &src[chg_start + k], sizeof(a));
            __builtin_memcpy(&b, &ref[chg_start + k], sizeof(b));
            d = a ^ b;
            __builtin_memcpy(&o[k], &d, sizeof(d));
        }
        for (; k < nchanged; k++)
            o[k] = src[chg_start + k] ^ ref[chg_start + k];
        o += nchanged;
        last_emit = i;
    }
    *o++ = REWIND_SENTINEL;
    out_buf = (uint8_t *)o;

    /* Tail bytes (state_size not a multiple of 4) — verbatim XOR delta. */
    for (size_t b = 0; b < tail_bytes; b++)
        out_buf[b] = src_tail[b] ^ ref_tail[b];
    out_buf += tail_bytes;

    return (size_t)(out_buf - out_start);
}

static bool decode_delta(uint32_t       * restrict dst,
                         const uint32_t * restrict ref,
                         const uint8_t  * restrict in,
                         size_t                    in_size,
                         size_t                    n_words,
                         uint8_t        * restrict dst_tail,
                         const uint8_t  * restrict ref_tail,
                         size_t                    tail_bytes)
{
    const uint32_t *p     = (const uint32_t *)in;
    const uint32_t *p_end = (const uint32_t *)(in + in_size);

    size_t i = 0;
    for (;;) {
        if (p >= p_end) return false;
        uint32_t skip = *p++;
        if (skip == REWIND_SENTINEL) break;
        if (p >= p_end) return false;
        uint32_t nchanged = *p++;
        i += skip;
        if (i + nchanged > n_words) return false;
        if (p + nchanged > p_end)   return false;

        size_t k     = 0;
        size_t v_run = nchanged & ~(size_t)3;
        for (; k < v_run; k += 4) {
            v_u32 r, pl, d;
            __builtin_memcpy(&r,  &ref[i + k], sizeof(r));
            __builtin_memcpy(&pl, &p[k],       sizeof(pl));
            d = r ^ pl;
            __builtin_memcpy(&dst[i + k], &d, sizeof(d));
        }
        for (; k < nchanged; k++)
            dst[i + k] = ref[i + k] ^ p[k];
        p += nchanged;
        i += nchanged;
    }

    if (tail_bytes) {
        const uint8_t *tin = (const uint8_t *)p;
        if (tin + tail_bytes > (const uint8_t *)p_end) return false;
        for (size_t b = 0; b < tail_bytes; b++)
            dst_tail[b] = ref_tail[b] ^ tin[b];
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Compute worst-case encoded delta size.  Pathological alternating runs
 * yield 12 bytes per word-pair = 1.5 * state_size_aligned, plus sentinel
 * and tail bytes.  Round up generously so caller can use this as an a
 * priori bound for "does this fit?" checks. */
static size_t worst_encoded_size(size_t state_size)
{
    size_t aligned = state_size & ~(size_t)3;
    size_t bound   = aligned + (aligned >> 1) + 32; /* 1.5x + sentinel slack */
    return bound + (state_size - aligned);          /* + tail bytes verbatim */
}

/* -------------------------------------------------------------------------
 * Create / destroy
 * ---------------------------------------------------------------------- */

libra_rewind_t *libra_rewind_create(unsigned max_slots, size_t max_bytes,
                                     size_t state_size)
{
    if (max_slots == 0 || max_bytes == 0 || state_size == 0)
        return NULL;

    libra_rewind_t *rw = calloc(1, sizeof(*rw));
    if (!rw) return NULL;

    rw->state_size       = state_size;
    rw->state_words      = state_size / sizeof(uint32_t);
    rw->tail_bytes       = state_size % sizeof(uint32_t);
    rw->slot_upper_bound = worst_encoded_size(state_size);
    rw->max_slots        = max_slots;
    rw->buf_size         = max_bytes;

    /* Ring must hold either a base slot (state_size) or a worst-case delta
     * (slot_upper_bound), whichever is bigger. */
    size_t min_required = state_size > rw->slot_upper_bound
                          ? state_size : rw->slot_upper_bound;
    if (max_bytes < min_required)
        goto fail;

    /* calloc the state buffers so the tail-of-last-word region (when
     * state_size is not a multiple of 4) is zero — and stays zero across
     * pushes since retro_serialize won't touch it.  Word compares therefore
     * never falsely fire on padding. */
    rw->buf_a = calloc(1, state_size);
    rw->buf_b = calloc(1, state_size);
    if (!rw->buf_a || !rw->buf_b) goto fail;
    rw->cur  = rw->buf_a;
    rw->prev = rw->buf_b;

    rw->buf = malloc(max_bytes);
    if (!rw->buf) goto fail;

    rw->slot_offsets = malloc(max_slots * sizeof(size_t));
    rw->slot_sizes   = malloc(max_slots * sizeof(size_t));
    rw->slot_is_base = malloc(max_slots * sizeof(bool));
    if (!rw->slot_offsets || !rw->slot_sizes || !rw->slot_is_base) goto fail;

    return rw;

fail:
    libra_rewind_destroy(rw);
    return NULL;
}

void libra_rewind_destroy(libra_rewind_t *rw)
{
    if (!rw) return;
    free(rw->buf_a);
    free(rw->buf_b);
    free(rw->buf);
    free(rw->slot_offsets);
    free(rw->slot_sizes);
    free(rw->slot_is_base);
    free(rw);
}

/* -------------------------------------------------------------------------
 * Accessors
 * ---------------------------------------------------------------------- */

void *libra_rewind_serialize_buf(libra_rewind_t *rw)
{
    /* Returns the buffer the frontend writes the next state into (before
     * push) AND reads the decoded state from (after pop).  The pointer
     * flips after every push, so the frontend MUST re-fetch it each frame
     * — caching across frames is undefined. */
    return rw ? rw->cur : NULL;
}

size_t libra_rewind_state_size(const libra_rewind_t *rw)
{
    return rw ? rw->state_size : 0;
}

unsigned libra_rewind_count(const libra_rewind_t *rw)
{
    return rw ? rw->count : 0;
}

/* -------------------------------------------------------------------------
 * Eviction — drop oldest slots until the target byte range and the slot
 * metadata ring both have room.
 *
 * Walks tail forward by slot index; bytes encoded in the new slot may
 * overlap older tail slots in the ring, but that's safe because eviction
 * is metadata-only (no read of slot bytes), and we run it after encoding
 * has already written.  Single-threaded: no races.
 * ---------------------------------------------------------------------- */

static void rewind_evict(libra_rewind_t *rw, size_t write_start, size_t write_end)
{
    while (rw->count > 0) {
        size_t ts = rw->slot_offsets[rw->tail];
        size_t te = ts + rw->slot_sizes[rw->tail];
        bool byte_overlap = (ts < write_end) && (te > write_start);
        bool meta_full    = (rw->count >= rw->max_slots);
        if (!byte_overlap && !meta_full)
            break;
        rw->tail = (rw->tail + 1) % rw->max_slots;
        rw->count--;
    }
}

/* -------------------------------------------------------------------------
 * Push — encode and store the contents of `cur`.
 *
 * Hot-path memory transactions:
 *   - one read of state_size from cur
 *   - one read of state_size from prev (delta path only)
 *   - one write of `actual_size` bytes to ring (encoded data)
 *   - NO state_size memcpy of "delta_ref" — eliminated by ping-pong
 *   - NO state_size memcpy from a scratch buffer to ring — eliminated by
 *     direct-encode
 * ---------------------------------------------------------------------- */

bool libra_rewind_push(libra_rewind_t *rw)
{
    if (!rw) return false;

    bool   is_base = (rw->count == 0);
    size_t upper   = is_base ? rw->state_size : rw->slot_upper_bound;

    if (upper > rw->buf_size)
        return false;

    /* Wrap to start if the worst-case encoding wouldn't fit at the current
     * head.  Bytes between the old buf_head and buf_size become dead until
     * tail catches up — slot_offsets[] records absolute positions so no
     * sentinel byte is needed in the ring itself. */
    if (rw->buf_head + upper > rw->buf_size)
        rw->buf_head = 0;

    /* Encode directly into the ring. */
    size_t actual_size;
    if (is_base) {
        memcpy(rw->buf + rw->buf_head, rw->cur, rw->state_size);
        actual_size = rw->state_size;
    } else {
        actual_size = encode_delta(
            (const uint32_t *)rw->cur,
            (const uint32_t *)rw->prev,
            rw->buf + rw->buf_head,
            rw->state_words,
            rw->cur  + rw->state_words * sizeof(uint32_t),
            rw->prev + rw->state_words * sizeof(uint32_t),
            rw->tail_bytes);
    }

    /* Now evict using the precise size — bytes were already written but
     * eviction only manipulates metadata, so older slots whose bytes we
     * just clobbered get cleanly removed. */
    rewind_evict(rw, rw->buf_head, rw->buf_head + actual_size);

    rw->slot_offsets[rw->head] = rw->buf_head;
    rw->slot_sizes[rw->head]   = actual_size;
    rw->slot_is_base[rw->head] = is_base;

    rw->buf_head += actual_size;
    rw->head      = (rw->head + 1) % rw->max_slots;
    rw->count++;

    /* Ping-pong: just-written state stays at its physical address; the
     * cur/prev labels swap so the next serialize_buf() call returns the
     * *other* buffer (next write target).  No state-sized memcpy. */
    uint8_t *tmp = rw->cur;
    rw->cur  = rw->prev;
    rw->prev = tmp;

    return true;
}

/* -------------------------------------------------------------------------
 * Pop — decode the newest slot back into cur.
 * ---------------------------------------------------------------------- */

size_t libra_rewind_pop(libra_rewind_t *rw)
{
    if (!rw || rw->count == 0)
        return 0;

    rw->head = (rw->head - 1 + rw->max_slots) % rw->max_slots;
    rw->count--;

    size_t offset  = rw->slot_offsets[rw->head];
    size_t comp_sz = rw->slot_sizes[rw->head];
    bool   is_base = rw->slot_is_base[rw->head];

    if (is_base) {
        if (comp_sz != rw->state_size) goto fail;
        memcpy(rw->cur, rw->buf + offset, rw->state_size);
    } else {
        /* Invariant on entry: prev holds the state that was current when
         * this slot was pushed.  cur holds either the prior-prior state
         * (post-push swap) or the same content as prev (post-pop sync).
         * Either way, every "skip" word in cur already matches the
         * desired output, so decode_delta only writes the changed words. */
        if (!decode_delta(
                (uint32_t *)rw->cur,
                (const uint32_t *)rw->prev,
                rw->buf + offset,
                comp_sz,
                rw->state_words,
                rw->cur  + rw->state_words * sizeof(uint32_t),
                rw->prev + rw->state_words * sizeof(uint32_t),
                rw->tail_bytes))
            goto fail;
    }

    /* Sync prev for the next pop's reference.  The state two snapshots
     * back is never raw in any buffer, so this memcpy is unavoidable
     * without an explicit triple-buffer + flag. */
    memcpy(rw->prev, rw->cur, rw->state_size);

    /* Restore buf_head to the end of the new newest slot so the next push
     * appends contiguously (subject to the wrap check there). */
    if (rw->count > 0) {
        unsigned newest = (rw->head - 1 + rw->max_slots) % rw->max_slots;
        rw->buf_head = rw->slot_offsets[newest] + rw->slot_sizes[newest];
        if (rw->buf_head >= rw->buf_size)
            rw->buf_head = 0;
    } else {
        rw->buf_head = 0;
    }

    return rw->state_size;

fail:
    rw->head = (rw->head + 1) % rw->max_slots;
    rw->count++;
    return 0;
}

/* -------------------------------------------------------------------------
 * Reset
 * ---------------------------------------------------------------------- */

void libra_rewind_reset(libra_rewind_t *rw)
{
    if (!rw) return;
    rw->head     = 0;
    rw->tail     = 0;
    rw->count    = 0;
    rw->buf_head = 0;
}
