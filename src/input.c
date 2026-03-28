// SPDX-License-Identifier: MIT
#include "input.h"
#include "libra_internal.h"
#include "libretro.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

void libra_input_poll(libra_ctx_t *ctx)
{
    /* Suppress polling during rollback replay — physical input is irrelevant */
    if (ctx->input_override_active)
        return;

    if (ctx->config.input_poll)
        ctx->config.input_poll(ctx->config.userdata);
}

int16_t libra_input_state(libra_ctx_t *ctx,
                           unsigned port, unsigned device,
                           unsigned index, unsigned id)
{
    /* Rollback replay: return stored input instead of polling host */
    if (ctx->input_override_active) {
        if ((device & 0xFF) == RETRO_DEVICE_JOYPAD && port < 16) {
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
                /* Remap the override bitmask */
                if (ctx->remap_loaded) {
                    uint32_t raw = ctx->input_override[port];
                    uint32_t out = 0;
                    for (unsigned btn = 0; btn < 16; btn++) {
                        if (raw & (1u << btn)) {
                            int8_t r = ctx->remap[port < 16 ? port : 0][btn];
                            out |= (1u << (unsigned)(r >= 0 ? (unsigned)r : btn));
                        }
                    }
                    return (int16_t)out;
                }
                return (int16_t)ctx->input_override[port];
            }
            /* Single button — remap id */
            unsigned rid = id;
            if (ctx->remap_loaded && port < 16 && id < 16) {
                int8_t r = ctx->remap[port][id];
                if (r >= 0) rid = (unsigned)r;
            }
            return (ctx->input_override[port] & (1u << rid)) ? 1 : 0;
        }
        return 0;
    }

    if (!ctx->config.input_state)
        return 0;

    /* Apply joypad remapping */
    if (device == RETRO_DEVICE_JOYPAD && ctx->remap_loaded && port < 16) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            /* Bitmask mode: query each button with remap applied */
            int16_t mask = 0;
            for (unsigned btn = RETRO_DEVICE_ID_JOYPAD_B;
                 btn <= RETRO_DEVICE_ID_JOYPAD_R3; btn++) {
                int8_t r = ctx->remap[port][btn];
                unsigned remapped = (r >= 0) ? (unsigned)r : btn;
                if (ctx->config.input_state(ctx->config.userdata,
                                            port, RETRO_DEVICE_JOYPAD,
                                            0, remapped))
                    mask |= (int16_t)(1 << btn);
            }
            return mask;
        }
        if (id < 16) {
            int8_t r = ctx->remap[port][id];
            unsigned remapped = (r >= 0) ? (unsigned)r : id;
            return ctx->config.input_state(ctx->config.userdata,
                                           port, device, index, remapped);
        }
    }

    /* Non-remapped bitmask synthesis for joypad without remap */
    if (device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        int16_t mask = 0;
        for (unsigned btn = RETRO_DEVICE_ID_JOYPAD_B;
             btn <= RETRO_DEVICE_ID_JOYPAD_R3; btn++) {
            if (ctx->config.input_state(ctx->config.userdata,
                                        port, RETRO_DEVICE_JOYPAD, 0, btn))
                mask |= (int16_t)(1 << btn);
        }
        return mask;
    }

    return ctx->config.input_state(ctx->config.userdata,
                                   port, device, index, id);
}

/* -------------------------------------------------------------------------
 * Input remapping
 * ---------------------------------------------------------------------- */

void libra_init_remaps(libra_ctx_t *ctx)
{
    if (!ctx) return;
    for (int p = 0; p < 16; p++)
        for (int b = 0; b < 16; b++)
            ctx->remap[p][b] = (int8_t)b;
    ctx->remap_loaded = true;
}

static int btn_name_to_id(const char *name)
{
    static const char *names[] = {
        "b", "y", "select", "start", "up", "down", "left", "right",
        "a", "x", "l", "r", "l2", "r2", "l3", "r3"
    };
    for (int i = 0; i < 16; i++)
        if (strcmp(name, names[i]) == 0) return i;
    return -1;
}

/* Load a .rmp remap file.
 *
 * Standard format (clean-room implementation of the well-known
 * key=value convention used by libretro frontends):
 *
 *   input_player1_btn_b = "0"
 *   input_player1_btn_a = "8"
 *   input_player2_btn_x = "9"
 *
 * Each line maps a physical button (left of =) to a logical
 * libretro RETRO_DEVICE_ID_JOYPAD_* value (right of =).
 * Player numbering is 1-based. Values may be quoted or bare.
 * Lines starting with # are comments. */

unsigned libra_load_remaps(libra_ctx_t *ctx, const char *path)
{
    if (!ctx || !path) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    /* Ensure identity baseline */
    if (!ctx->remap_loaded)
        libra_init_remaps(ctx);

    char line[256];
    unsigned applied = 0;
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'
                           || line[len-1] == ' '))
            line[--len] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *lhs = line;
        const char *rhs = eq + 1;
        while (*rhs == ' ') rhs++;
        /* Trim trailing spaces from lhs */
        {
            int llen = (int)strlen(lhs);
            char *llhs = (char*)lhs;
            while (llen > 0 && llhs[llen-1] == ' ') llhs[--llen] = '\0';
        }

        /* Parse: input_player<N>_btn_<name> = ["]<id>["] */
        int player = 0;
        char btn_name[32] = {0};
        if (sscanf(lhs, "input_player%d_btn_%31s", &player, btn_name) != 2)
            continue;
        if (player < 1 || player > 16)
            continue;

        /* Trim trailing spaces from btn_name */
        for (int i = (int)strlen(btn_name) - 1; i >= 0 && btn_name[i] == ' '; i--)
            btn_name[i] = '\0';

        /* Parse value: may be quoted ("8") or bare (8) */
        int dst_id = (rhs[0] == '"') ? atoi(rhs + 1) : atoi(rhs);

        int src = btn_name_to_id(btn_name);
        if (src < 0 || dst_id < 0 || dst_id > 15)
            continue;

        int port = player - 1; /* 1-based → 0-based */
        ctx->remap[port][src] = (int8_t)dst_id;
        applied++;
    }
    fclose(f);

    return applied;
}
