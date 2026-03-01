// SPDX-License-Identifier: MIT
#ifndef LIBRA_REWIND_H
#define LIBRA_REWIND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct libra_rewind libra_rewind_t;

libra_rewind_t *libra_rewind_create(unsigned capacity);
void            libra_rewind_destroy(libra_rewind_t *rw);

/* Store a compressed snapshot. raw/raw_size = uncompressed state. */
bool libra_rewind_push(libra_rewind_t *rw, const void *raw, size_t raw_size);

/* Decompress the most recent snapshot into buf (must be >= orig_size).
 * Returns original uncompressed size, or 0 on failure / empty. */
size_t libra_rewind_pop(libra_rewind_t *rw, void *buf, size_t buf_size);

unsigned libra_rewind_count(const libra_rewind_t *rw);

#endif /* LIBRA_REWIND_H */
