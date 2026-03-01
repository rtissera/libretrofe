// SPDX-License-Identifier: MIT
#include "input.h"
#include "libra_internal.h"
#include "libretro.h"

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
    if (ctx->input_override_active && (device & 0xFF) == RETRO_DEVICE_JOYPAD) {
        if (port < 16) {
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
                return (int16_t)ctx->input_override[port];
            return (ctx->input_override[port] & (1u << id)) ? 1 : 0;
        }
        return 0;
    }

    if (!ctx->config.input_state)
        return 0;

    /* When the core uses bitmask mode it calls input_state with
     * id == RETRO_DEVICE_ID_JOYPAD_MASK and expects a packed bitmask
     * of all joypad buttons.  Synthesise it from individual queries so
     * the host callback never has to know about the bitmask protocol. */
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
