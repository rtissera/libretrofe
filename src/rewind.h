// SPDX-License-Identifier: MIT
#ifndef LIBRA_REWIND_H
#define LIBRA_REWIND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct libra_rewind libra_rewind_t;

/* Create a rewind ring buffer.
 * capacity  = number of snapshot slots
 * state_size = retro_serialize_size() — used to pre-allocate all buffers.
 *              Must be > 0.  All per-frame malloc/free is eliminated. */
libra_rewind_t *libra_rewind_create(unsigned capacity, size_t state_size);
void            libra_rewind_destroy(libra_rewind_t *rw);

/* Return pointer to the pre-allocated serialize buffer (state_size bytes).
 * The caller serializes directly into this buffer, then calls
 * libra_rewind_push() to compress and store it. */
void  *libra_rewind_serialize_buf(libra_rewind_t *rw);
size_t  libra_rewind_state_size(const libra_rewind_t *rw);

/* Compress the contents of the serialize buffer and store as a snapshot.
 * Returns true on success. */
bool libra_rewind_push(libra_rewind_t *rw);

/* Decompress the most recent snapshot into the serialize buffer.
 * Returns original uncompressed size, or 0 on failure / empty.
 * The caller can then pass the buffer to retro_unserialize(). */
size_t libra_rewind_pop(libra_rewind_t *rw);

unsigned libra_rewind_count(const libra_rewind_t *rw);

#endif /* LIBRA_REWIND_H */
