// SPDX-License-Identifier: MIT
#include "rewind.h"
#include "zstd.h"
#include <stdlib.h>
#include <string.h>

struct libra_rewind {
    /* Serialize buffer — retro_serialize writes here; push/pop read/write here */
    uint8_t   *serialize_buf;
    size_t     state_size;

    /* XOR delta support — zero per-frame allocation */
    uint8_t   *xor_buf;    /* scratch: XOR(current, delta_ref) before compress  */
    uint8_t   *delta_ref;  /* last pushed full state; used as XOR reference      */

    /* Compression scratch — reused every push, never stored in the ring */
    uint8_t   *compress_buf;
    size_t     compress_buf_size;   /* ZSTD_compressBound(state_size) */

    /* Persistent zstd contexts — avoids per-frame alloc inside zstd */
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;

    /* -----------------------------------------------------------------------
     * Circular byte buffer — all compressed slot data lives here.
     *
     * Layout at any point:
     *   - Slots are written sequentially from buf_head.
     *   - When an entry doesn't fit at the end, buf_head wraps to 0 and
     *     the remaining bytes at the old position are silently skipped.
     *   - slot_offsets[] records where each slot's data actually starts,
     *     so pop() and eviction never need to scan the byte buffer.
     * -------------------------------------------------------------------- */
    uint8_t   *buf;
    size_t     buf_size;    /* total capacity in bytes (max_bytes) */
    size_t     buf_head;    /* byte offset of next write */

    /* Slot metadata — parallel pre-allocated arrays, indexed by the slot ring */
    size_t    *slot_offsets;    /* buf offset where each slot's compressed data begins */
    size_t    *slot_sizes;      /* actual compressed bytes stored */
    bool      *slot_is_base;    /* true → full state (first slot); false → XOR delta */

    unsigned   max_slots;
    unsigned   head;   /* next write index in the slot ring */
    unsigned   tail;   /* oldest valid slot index */
    unsigned   count;  /* number of valid slots currently stored */
};

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

    rw->state_size = state_size;
    rw->max_slots  = max_slots;
    rw->buf_size   = max_bytes;

    rw->serialize_buf = malloc(state_size);
    if (!rw->serialize_buf) goto fail;

    rw->xor_buf = malloc(state_size);
    if (!rw->xor_buf) goto fail;

    /* delta_ref is zeroed; the first push marks is_base=true so the zero
     * reference is never used during a pop — safe regardless. */
    rw->delta_ref = calloc(1, state_size);
    if (!rw->delta_ref) goto fail;

    rw->compress_buf_size = ZSTD_compressBound(state_size);
    rw->compress_buf = malloc(rw->compress_buf_size);
    if (!rw->compress_buf) goto fail;

    rw->cctx = ZSTD_createCCtx();
    rw->dctx = ZSTD_createDCtx();
    if (!rw->cctx || !rw->dctx) goto fail;

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
    free(rw->serialize_buf);
    free(rw->xor_buf);
    free(rw->delta_ref);
    free(rw->compress_buf);
    if (rw->cctx) ZSTD_freeCCtx(rw->cctx);
    if (rw->dctx) ZSTD_freeDCtx(rw->dctx);
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
    return rw ? rw->serialize_buf : NULL;
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
 * Push
 * ---------------------------------------------------------------------- */

bool libra_rewind_push(libra_rewind_t *rw)
{
    if (!rw) return false;

    /* First slot stored as a full state so pop() can always reconstruct it
     * without needing an earlier delta reference. */
    bool is_base = (rw->count == 0);

    size_t actual_size;
    if (is_base) {
        actual_size = ZSTD_compressCCtx(rw->cctx,
                          rw->compress_buf, rw->compress_buf_size,
                          rw->serialize_buf, rw->state_size, 1);
    } else {
        /* XOR delta: mostly zeros when inter-frame state change is small,
         * which compresses far better than the full state on zstd level 1. */
        const uint8_t *src = (const uint8_t *)rw->serialize_buf;
        const uint8_t *ref = (const uint8_t *)rw->delta_ref;
        uint8_t       *xb  = (uint8_t *)rw->xor_buf;
        size_t n = rw->state_size;
        for (size_t i = 0; i < n; i++)
            xb[i] = src[i] ^ ref[i];
        actual_size = ZSTD_compressCCtx(rw->cctx,
                          rw->compress_buf, rw->compress_buf_size,
                          rw->xor_buf, rw->state_size, 1);
    }

    if (ZSTD_isError(actual_size) || actual_size > rw->buf_size)
        return false;

    /* Wrap buf_head to 0 when the entry doesn't fit at the current position.
     * Bytes between the old buf_head and buf_size are silently skipped;
     * slot_offsets[] records actual positions so no sentinel is needed. */
    if (rw->buf_head + actual_size > rw->buf_size)
        rw->buf_head = 0;

    /* Evict oldest slots whose byte range overlaps the target write region,
     * or when the slot metadata ring would overflow.
     *
     * Overlap condition: [slot_start, slot_end) ∩ [buf_head, buf_head+size)
     * This works for both the linear case and after a buf_head wrap because
     * slot_offsets[] records absolute byte positions inside buf[]. */
    size_t write_end = rw->buf_head + actual_size;
    while (rw->count > 0) {
        size_t ts = rw->slot_offsets[rw->tail];
        size_t te = ts + rw->slot_sizes[rw->tail];
        bool byte_overlap = (ts < write_end) && (te > rw->buf_head);
        bool meta_full    = (rw->count >= rw->max_slots);
        if (!byte_overlap && !meta_full)
            break;
        rw->tail = (rw->tail + 1) % rw->max_slots;
        rw->count--;
    }

    /* Write compressed data into the circular byte buffer */
    memcpy(rw->buf + rw->buf_head, rw->compress_buf, actual_size);
    rw->slot_offsets[rw->head] = rw->buf_head;
    rw->slot_sizes[rw->head]   = actual_size;
    rw->slot_is_base[rw->head] = is_base;

    rw->buf_head = rw->buf_head + actual_size;
    rw->head     = (rw->head + 1) % rw->max_slots;
    rw->count++;

    /* Update delta reference to the state we just stored */
    memcpy(rw->delta_ref, rw->serialize_buf, rw->state_size);

    return true;
}

/* -------------------------------------------------------------------------
 * Pop
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
        /* Full state — decompress directly into serialize_buf */
        size_t got = ZSTD_decompressDCtx(rw->dctx,
                         rw->serialize_buf, rw->state_size,
                         rw->buf + offset, comp_sz);
        if (ZSTD_isError(got)) goto fail;
    } else {
        /* XOR delta — decompress into xor_buf, then XOR with delta_ref.
         * Invariant: delta_ref == the state restored by the previous pop
         * (or the last pushed state if no pop has occurred yet), which is
         * exactly the reference used when this delta was computed. */
        size_t got = ZSTD_decompressDCtx(rw->dctx,
                         rw->xor_buf, rw->state_size,
                         rw->buf + offset, comp_sz);
        if (ZSTD_isError(got)) goto fail;
        const uint8_t *xb  = (const uint8_t *)rw->xor_buf;
        const uint8_t *ref = (const uint8_t *)rw->delta_ref;
        uint8_t       *out = (uint8_t *)rw->serialize_buf;
        size_t n = rw->state_size;
        for (size_t i = 0; i < n; i++)
            out[i] = xb[i] ^ ref[i];
    }

    /* delta_ref becomes the state we just restored, ready for the next pop */
    memcpy(rw->delta_ref, rw->serialize_buf, rw->state_size);

    /* Restore buf_head to the end of what is now the newest slot.
     * This is where the next push will write (after a potential wrap check). */
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
    /* Restore head/count on decompression error — ring is unchanged */
    rw->head = (rw->head + 1) % rw->max_slots;
    rw->count++;
    return 0;
}

/* -------------------------------------------------------------------------
 * Reset (flush without freeing)
 * ---------------------------------------------------------------------- */

void libra_rewind_reset(libra_rewind_t *rw)
{
    if (!rw) return;
    rw->head     = 0;
    rw->tail     = 0;
    rw->count    = 0;
    rw->buf_head = 0;
    /* Zero delta_ref so the next push stores a clean full state as base */
    memset(rw->delta_ref, 0, rw->state_size);
}
