// SPDX-License-Identifier: MIT
#include "rewind.h"
#include "zstd.h"
#include <stdlib.h>
#include <string.h>

struct libra_rewind {
    /* Pre-allocated serialize buffer (retro_serialize target) */
    uint8_t   *serialize_buf;
    size_t     state_size;       /* retro_serialize_size(), fixed per core */

    /* Pre-allocated compression output buffer */
    uint8_t   *compress_buf;
    size_t     compress_buf_size;

    /* Persistent zstd contexts (avoid per-frame alloc inside zstd) */
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;

    /* Ring buffer of compressed snapshots.
     * Each slot is a fixed-size region inside a single contiguous arena.
     * comp_sizes[i] stores the actual compressed bytes in that slot. */
    uint8_t   *arena;            /* capacity * slot_size contiguous bytes */
    size_t     slot_size;        /* ZSTD_compressBound(state_size) */
    size_t    *comp_sizes;       /* actual compressed size per slot */

    unsigned   capacity;
    unsigned   head;             /* next write position */
    unsigned   count;            /* number of filled slots */
};

libra_rewind_t *libra_rewind_create(unsigned capacity, size_t state_size)
{
    if (capacity == 0 || state_size == 0)
        return NULL;

    libra_rewind_t *rw = calloc(1, sizeof(*rw));
    if (!rw)
        return NULL;

    rw->state_size = state_size;
    rw->capacity   = capacity;

    /* Pre-allocate serialize buffer */
    rw->serialize_buf = malloc(state_size);
    if (!rw->serialize_buf)
        goto fail;

    /* Pre-allocate compression output buffer */
    rw->compress_buf_size = ZSTD_compressBound(state_size);
    rw->compress_buf = malloc(rw->compress_buf_size);
    if (!rw->compress_buf)
        goto fail;

    /* Create persistent zstd contexts */
    rw->cctx = ZSTD_createCCtx();
    rw->dctx = ZSTD_createDCtx();
    if (!rw->cctx || !rw->dctx)
        goto fail;

    /* Pre-allocate contiguous arena for all ring slots */
    rw->slot_size  = rw->compress_buf_size;
    rw->arena      = malloc((size_t)capacity * rw->slot_size);
    rw->comp_sizes = calloc(capacity, sizeof(size_t));
    if (!rw->arena || !rw->comp_sizes)
        goto fail;

    return rw;

fail:
    libra_rewind_destroy(rw);
    return NULL;
}

void libra_rewind_destroy(libra_rewind_t *rw)
{
    if (!rw)
        return;
    free(rw->serialize_buf);
    free(rw->compress_buf);
    if (rw->cctx) ZSTD_freeCCtx(rw->cctx);
    if (rw->dctx) ZSTD_freeDCtx(rw->dctx);
    free(rw->arena);
    free(rw->comp_sizes);
    free(rw);
}

void *libra_rewind_serialize_buf(libra_rewind_t *rw)
{
    return rw ? rw->serialize_buf : NULL;
}

size_t libra_rewind_state_size(const libra_rewind_t *rw)
{
    return rw ? rw->state_size : 0;
}

bool libra_rewind_push(libra_rewind_t *rw)
{
    if (!rw)
        return false;

    /* Compress serialize_buf → compress_buf using persistent context */
    size_t comp_sz = ZSTD_compressCCtx(rw->cctx,
                                        rw->compress_buf, rw->compress_buf_size,
                                        rw->serialize_buf, rw->state_size,
                                        1 /* level 1 = fastest */);
    if (ZSTD_isError(comp_sz))
        return false;

    /* Copy compressed data into the ring slot */
    uint8_t *slot = rw->arena + (size_t)rw->head * rw->slot_size;
    memcpy(slot, rw->compress_buf, comp_sz);
    rw->comp_sizes[rw->head] = comp_sz;

    rw->head = (rw->head + 1) % rw->capacity;
    if (rw->count < rw->capacity)
        rw->count++;

    return true;
}

size_t libra_rewind_pop(libra_rewind_t *rw)
{
    if (!rw || rw->count == 0)
        return 0;

    /* Move head back to the most recent slot */
    rw->head = (rw->head - 1 + rw->capacity) % rw->capacity;
    rw->count--;

    uint8_t *slot = rw->arena + (size_t)rw->head * rw->slot_size;
    size_t comp_sz = rw->comp_sizes[rw->head];

    /* Decompress directly into serialize_buf using persistent context */
    size_t orig = ZSTD_decompressDCtx(rw->dctx,
                                       rw->serialize_buf, rw->state_size,
                                       slot, comp_sz);
    if (ZSTD_isError(orig))
        return 0;

    rw->comp_sizes[rw->head] = 0;
    return orig;
}

unsigned libra_rewind_count(const libra_rewind_t *rw)
{
    return rw ? rw->count : 0;
}
