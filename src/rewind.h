// SPDX-License-Identifier: MIT
#ifndef LIBRA_REWIND_H
#define LIBRA_REWIND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct libra_rewind libra_rewind_t;

/* Create a rewind ring buffer.
 *
 * max_slots  — maximum number of snapshots kept at once.  A slot
 *              is one compressed frame; older slots are evicted when
 *              either max_slots or max_bytes would be exceeded.
 * max_bytes  — total byte budget for the circular data buffer.
 *              Must be >= ZSTD_compressBound(state_size) so that even
 *              a worst-case full-state snapshot fits.
 * state_size — retro_serialize_size().  Fixed for the life of the ring.
 *
 * All allocations happen here; push/pop/flush touch no heap. */
libra_rewind_t *libra_rewind_create(unsigned max_slots, size_t max_bytes,
                                     size_t state_size);
void            libra_rewind_destroy(libra_rewind_t *rw);

/* Return the pre-allocated serialize buffer (state_size bytes).
 * The caller writes the serialized core state here, then calls push(). */
void  *libra_rewind_serialize_buf(libra_rewind_t *rw);
size_t  libra_rewind_state_size(const libra_rewind_t *rw);

/* Push: compress and store the current contents of serialize_buf.
 * First snapshot is stored as a full state; subsequent ones as XOR deltas
 * against the previous snapshot, which compress far better on slow hardware.
 * Oldest slots are evicted as needed to respect max_slots / max_bytes.
 * Returns true on success. */
bool libra_rewind_push(libra_rewind_t *rw);

/* Pop: decompress the most recent snapshot back into serialize_buf.
 * Returns state_size on success, 0 on failure or empty ring.
 * The caller passes serialize_buf to retro_unserialize(). */
size_t libra_rewind_pop(libra_rewind_t *rw);

/* Reset the ring to empty without freeing memory.
 * Call whenever the game state changes discontinuously (disk state load,
 * core reset) so stale delta references cannot corrupt future pops. */
void libra_rewind_reset(libra_rewind_t *rw);

unsigned libra_rewind_count(const libra_rewind_t *rw);

#endif /* LIBRA_REWIND_H */
