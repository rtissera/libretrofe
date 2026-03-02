// SPDX-License-Identifier: MIT
#include "input.h"
#include "libra_internal.h"
#include "libretro.h"

#include <string.h>
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

        int port = 0;
        if (lhs[0] == 'p' && lhs[1] == 'o' && lhs[2] == 'r' && lhs[3] == 't'
            && lhs[4] >= '0' && lhs[4] <= '9') {
            port = lhs[4] - '0';
            lhs = lhs + 6;
        }

        while (*lhs == ' ') lhs++;
        char src_buf[32] = {0}, dst_buf[32] = {0};
        if (sscanf(lhs, "joypad_%31s", src_buf) != 1) continue;
        if (sscanf(rhs, "joypad_%31s", dst_buf) != 1) continue;

        /* Trim trailing spaces from src_buf */
        for (int i = (int)strlen(src_buf) - 1; i >= 0 && src_buf[i] == ' '; i--)
            src_buf[i] = '\0';

        int src = btn_name_to_id(src_buf);
        int dst = btn_name_to_id(dst_buf);
        if (src >= 0 && dst >= 0 && port < 16) {
            ctx->remap[port][src] = (int8_t)dst;
            applied++;
        }
    }
    fclose(f);

    return applied;
}
