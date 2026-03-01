// SPDX-License-Identifier: MIT
#include "rewind.h"
#include "miniz.h"
#include <stdlib.h>
#include <string.h>

struct libra_rewind {
    uint8_t  **slots;       /* compressed data per slot */
    size_t    *comp_sizes;  /* compressed size per slot */
    size_t    *orig_sizes;  /* original uncompressed size per slot */
    unsigned   capacity;
    unsigned   head;        /* next write position */
    unsigned   count;       /* number of filled slots */
};

libra_rewind_t *libra_rewind_create(unsigned capacity)
{
    if (capacity == 0)
        return NULL;

    libra_rewind_t *rw = calloc(1, sizeof(*rw));
    if (!rw)
        return NULL;

    rw->slots      = calloc(capacity, sizeof(uint8_t *));
    rw->comp_sizes = calloc(capacity, sizeof(size_t));
    rw->orig_sizes = calloc(capacity, sizeof(size_t));
    rw->capacity   = capacity;

    if (!rw->slots || !rw->comp_sizes || !rw->orig_sizes) {
        free(rw->slots);
        free(rw->comp_sizes);
        free(rw->orig_sizes);
        free(rw);
        return NULL;
    }

    return rw;
}

void libra_rewind_destroy(libra_rewind_t *rw)
{
    if (!rw)
        return;
    for (unsigned i = 0; i < rw->capacity; i++)
        free(rw->slots[i]);
    free(rw->slots);
    free(rw->comp_sizes);
    free(rw->orig_sizes);
    free(rw);
}

bool libra_rewind_push(libra_rewind_t *rw, const void *raw, size_t raw_size)
{
    if (!rw || !raw || raw_size == 0)
        return false;

    uLong bound = compressBound((uLong)raw_size);
    uint8_t *buf = malloc(bound);
    if (!buf)
        return false;

    uLong comp_sz = bound;
    if (compress2(buf, &comp_sz, (const Bytef *)raw, (uLong)raw_size,
                  Z_BEST_SPEED) != Z_OK) {
        free(buf);
        return false;
    }

    /* Shrink to actual size */
    uint8_t *shrunk = realloc(buf, comp_sz);
    if (shrunk)
        buf = shrunk;

    /* Free old slot at head (if ring is full, this overwrites oldest) */
    free(rw->slots[rw->head]);

    rw->slots[rw->head]      = buf;
    rw->comp_sizes[rw->head] = comp_sz;
    rw->orig_sizes[rw->head] = raw_size;

    rw->head = (rw->head + 1) % rw->capacity;
    if (rw->count < rw->capacity)
        rw->count++;

    return true;
}

size_t libra_rewind_pop(libra_rewind_t *rw, void *buf, size_t buf_size)
{
    if (!rw || rw->count == 0 || !buf)
        return 0;

    /* Move head back to the most recent slot */
    rw->head = (rw->head - 1 + rw->capacity) % rw->capacity;
    rw->count--;

    size_t orig = rw->orig_sizes[rw->head];
    if (buf_size < orig)
        return 0;

    uLong dest_len = (uLong)buf_size;
    if (uncompress((Bytef *)buf, &dest_len,
                   rw->slots[rw->head], (uLong)rw->comp_sizes[rw->head]) != Z_OK)
        return 0;

    free(rw->slots[rw->head]);
    rw->slots[rw->head]      = NULL;
    rw->comp_sizes[rw->head] = 0;
    rw->orig_sizes[rw->head] = 0;

    return (size_t)dest_len;
}

unsigned libra_rewind_count(const libra_rewind_t *rw)
{
    return rw ? rw->count : 0;
}
