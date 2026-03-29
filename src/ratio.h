// SPDX-License-Identifier: MIT
#ifndef LIBRA_RATIO_H
#define LIBRA_RATIO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *label;
    const char *value;
    float       ratio;   /* 0 for non-numeric entries like "core", "custom" */
} libra_ratio_entry_t;

/* Returns a pointer to the built-in ratio table.
 * The table is terminated by an entry with label == NULL.
 * Includes numeric ratios (4:3, 16:9, ...) and special
 * entries (config, squarepixel, core, custom, full). */
const libra_ratio_entry_t *libra_get_ratios(void);

/* Returns the number of entries (not counting the terminator). */
int libra_get_ratio_count(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBRA_RATIO_H */
