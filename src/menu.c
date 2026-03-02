// SPDX-License-Identifier: MIT
#include "libra_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Options menu state machine
 * ---------------------------------------------------------------------- */

#define MENU_MAX_VISIBLE 8
#define MENU_LABEL_SIZE  256
#define MENU_VALUE_SIZE  128

struct libra_options_menu {
    libra_ctx_t *ctx;
    int level;      /* 0 = categories, 1 = options */
    int cat_sel;    /* selected category in level 0 */
    int opt_sel;    /* selected option in level 1 */
    int sel;        /* flat mode selection (no categories) */

    /* Filtered option indices (level 1) */
    unsigned filtered[LIBRA_MAX_OPTIONS];
    unsigned filtered_count;

    /* Cached visible items for rendering */
    struct {
        char label[MENU_LABEL_SIZE];
        char value[MENU_VALUE_SIZE];
        bool selected;
    } items[MENU_MAX_VISIBLE];
    unsigned item_count;
    int      scroll_offset;
    int      total_count;
};

/* Rebuild the filtered option list for the current category */
static void rebuild_filtered(libra_options_menu_t *m)
{
    libra_ctx_t *ctx = m->ctx;
    unsigned total = ctx->opt_count;
    unsigned cat_count = ctx->opt_cat_count;
    int target_cat = (m->cat_sel < (int)cat_count) ? m->cat_sel : -1;

    m->filtered_count = 0;
    for (unsigned i = 0; i < total; i++) {
        int ci = ctx->opt_cat_idx[i];
        if (ci == target_cat) {
            const char *key = ctx->opt_keys[i];
            if (key && libra_is_option_visible(ctx, key))
                m->filtered[m->filtered_count++] = i;
        }
    }
}

/* Check if there are uncategorised options */
static bool has_uncategorised(libra_ctx_t *ctx)
{
    for (unsigned i = 0; i < ctx->opt_count; i++) {
        if (ctx->opt_cat_idx[i] == -1)
            return true;
    }
    return false;
}

/* Rebuild the visible item list based on current state */
static void rebuild_items(libra_options_menu_t *m)
{
    libra_ctx_t *ctx = m->ctx;
    unsigned total = ctx->opt_count;
    unsigned cat_count = ctx->opt_cat_count;

    m->item_count = 0;
    m->scroll_offset = 0;
    m->total_count = 0;

    if (total == 0) {
        snprintf(m->items[0].label, MENU_LABEL_SIZE, "No core options");
        m->items[0].value[0] = '\0';
        m->items[0].selected = true;
        m->item_count = 1;
        m->total_count = 1;
        return;
    }

    if (cat_count == 0) {
        /* Flat mode (v1 core, no categories) */
        if ((unsigned)m->sel >= total) m->sel = 0;
        m->total_count = (int)total;
        int scroll = m->sel - MENU_MAX_VISIBLE + 1;
        if (scroll < 0) scroll = 0;
        m->scroll_offset = scroll;
        int end = m->total_count;
        if (end > scroll + MENU_MAX_VISIBLE) end = scroll + MENU_MAX_VISIBLE;

        for (int i = scroll; i < end; i++) {
            const char *desc = ctx->opt_descs[i];
            const char *key  = ctx->opt_keys[i];
            const char *val  = ctx->opt_vals[i];
            bool vis = key ? libra_is_option_visible(ctx, key) : true;
            if (!vis) continue;

            unsigned idx = m->item_count;
            if (idx >= MENU_MAX_VISIBLE) break;

            const char *label = (desc && desc[0]) ? desc : (key ? key : "?");
            snprintf(m->items[idx].label, MENU_LABEL_SIZE, "%s", label);
            snprintf(m->items[idx].value, MENU_VALUE_SIZE, "%s",
                     val ? val : "?");
            m->items[idx].selected = (i == m->sel);
            m->item_count++;
        }
        return;
    }

    if (m->level == 0) {
        /* Category list */
        bool uncat = has_uncategorised(ctx);
        m->total_count = (int)cat_count + (uncat ? 1 : 0);
        if (m->cat_sel >= m->total_count) m->cat_sel = 0;

        int scroll = m->cat_sel - MENU_MAX_VISIBLE + 1;
        if (scroll < 0) scroll = 0;
        m->scroll_offset = scroll;
        int end = m->total_count;
        if (end > scroll + MENU_MAX_VISIBLE) end = scroll + MENU_MAX_VISIBLE;

        for (int i = scroll; i < end; i++) {
            unsigned idx = m->item_count;
            if (idx >= MENU_MAX_VISIBLE) break;

            if (i < (int)cat_count) {
                const char *d = ctx->opt_cat_descs[i];
                snprintf(m->items[idx].label, MENU_LABEL_SIZE, "%s",
                         d ? d : "???");
            } else {
                snprintf(m->items[idx].label, MENU_LABEL_SIZE, "Other");
            }
            m->items[idx].value[0] = '\0'; /* categories have no value */
            m->items[idx].selected = (i == m->cat_sel);
            m->item_count++;
        }
        return;
    }

    /* Level 1: options in selected category */
    rebuild_filtered(m);
    m->total_count = (int)m->filtered_count;

    if (m->total_count == 0) {
        snprintf(m->items[0].label, MENU_LABEL_SIZE, "(empty)");
        m->items[0].value[0] = '\0';
        m->items[0].selected = true;
        m->item_count = 1;
        m->total_count = 1;
        return;
    }

    if (m->opt_sel >= m->total_count) m->opt_sel = m->total_count - 1;

    int scroll = m->opt_sel - MENU_MAX_VISIBLE + 1;
    if (scroll < 0) scroll = 0;
    m->scroll_offset = scroll;
    int end = m->total_count;
    if (end > scroll + MENU_MAX_VISIBLE) end = scroll + MENU_MAX_VISIBLE;

    for (int i = scroll; i < end; i++) {
        unsigned idx = m->item_count;
        if (idx >= MENU_MAX_VISIBLE) break;

        unsigned oi = m->filtered[i];
        const char *desc = ctx->opt_descs[oi];
        const char *key  = ctx->opt_keys[oi];
        const char *val  = ctx->opt_vals[oi];

        const char *label = (desc && desc[0]) ? desc : (key ? key : "?");
        snprintf(m->items[idx].label, MENU_LABEL_SIZE, "%s", label);
        snprintf(m->items[idx].value, MENU_VALUE_SIZE, "%s",
                 val ? val : "?");
        m->items[idx].selected = (i == m->opt_sel);
        m->item_count++;
    }
}

/* Cycle the currently selected option value */
static bool cycle_option(libra_options_menu_t *m, int direction)
{
    libra_ctx_t *ctx = m->ctx;
    unsigned cat_count = ctx->opt_cat_count;

    if (cat_count == 0) {
        /* Flat mode */
        return libra_option_cycle(ctx, (unsigned)m->sel, direction);
    }

    /* Categorised mode */
    rebuild_filtered(m);
    if (m->opt_sel >= 0 && m->opt_sel < (int)m->filtered_count)
        return libra_option_cycle(ctx, m->filtered[m->opt_sel], direction);

    return false;
}

/* ---- Public API -------------------------------------------------------- */

libra_options_menu_t *libra_options_menu_create(libra_ctx_t *ctx)
{
    if (!ctx) return NULL;
    libra_options_menu_t *m = (libra_options_menu_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->ctx = ctx;
    rebuild_items(m);
    return m;
}

void libra_options_menu_destroy(libra_options_menu_t *menu)
{
    free(menu);
}

int libra_options_menu_input(libra_options_menu_t *m, int action)
{
    if (!m) return 1;

    libra_ctx_t *ctx = m->ctx;
    unsigned total = ctx->opt_count;
    unsigned cat_count = ctx->opt_cat_count;
    int result = 0;

    if (action == LIBRA_MENU_BACK) {
        if (cat_count > 0 && m->level == 1) {
            m->level = 0;
            rebuild_items(m);
            return 0;
        }
        return 1; /* close menu */
    }

    if (cat_count == 0) {
        /* Flat mode */
        switch (action) {
        case LIBRA_MENU_UP:
            if (total > 0)
                m->sel = (m->sel - 1 + (int)total) % (int)total;
            break;
        case LIBRA_MENU_DOWN:
            if (total > 0)
                m->sel = (m->sel + 1) % (int)total;
            break;
        case LIBRA_MENU_LEFT:
            if (cycle_option(m, -1)) result = 2;
            break;
        case LIBRA_MENU_RIGHT:
        case LIBRA_MENU_ACCEPT:
            if (cycle_option(m, +1)) result = 2;
            break;
        }
        rebuild_items(m);
        return result;
    }

    if (m->level == 0) {
        /* Category list */
        bool uncat = has_uncategorised(ctx);
        int count = (int)cat_count + (uncat ? 1 : 0);
        switch (action) {
        case LIBRA_MENU_UP:
            m->cat_sel = (m->cat_sel - 1 + count) % count;
            break;
        case LIBRA_MENU_DOWN:
            m->cat_sel = (m->cat_sel + 1) % count;
            break;
        case LIBRA_MENU_ACCEPT:
        case LIBRA_MENU_RIGHT:
            m->level = 1;
            m->opt_sel = 0;
            break;
        default:
            break;
        }
    } else {
        /* Level 1: options in category */
        rebuild_filtered(m);
        int count = (int)m->filtered_count;
        if (count == 0) count = 1;
        switch (action) {
        case LIBRA_MENU_UP:
            m->opt_sel = (m->opt_sel - 1 + count) % count;
            break;
        case LIBRA_MENU_DOWN:
            m->opt_sel = (m->opt_sel + 1) % count;
            break;
        case LIBRA_MENU_LEFT:
            if (cycle_option(m, -1)) result = 2;
            break;
        case LIBRA_MENU_RIGHT:
        case LIBRA_MENU_ACCEPT:
            if (cycle_option(m, +1)) result = 2;
            break;
        }
    }

    rebuild_items(m);
    return result;
}

unsigned libra_options_menu_level(const libra_options_menu_t *m)
{
    return m ? (unsigned)m->level : 0;
}

unsigned libra_options_menu_category_count(const libra_options_menu_t *m)
{
    return m ? m->ctx->opt_cat_count : 0;
}

unsigned libra_options_menu_item_count(const libra_options_menu_t *m)
{
    return m ? m->item_count : 0;
}

const char *libra_options_menu_item_label(const libra_options_menu_t *m,
                                           unsigned i)
{
    if (!m || i >= m->item_count) return NULL;
    return m->items[i].label;
}

const char *libra_options_menu_item_value(const libra_options_menu_t *m,
                                           unsigned i)
{
    if (!m || i >= m->item_count) return NULL;
    if (m->items[i].value[0] == '\0') return NULL; /* categories */
    return m->items[i].value;
}

bool libra_options_menu_item_selected(const libra_options_menu_t *m,
                                       unsigned i)
{
    if (!m || i >= m->item_count) return false;
    return m->items[i].selected;
}

int libra_options_menu_scroll_offset(const libra_options_menu_t *m)
{
    return m ? m->scroll_offset : 0;
}

int libra_options_menu_total_count(const libra_options_menu_t *m)
{
    return m ? m->total_count : 0;
}
