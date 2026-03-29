// SPDX-License-Identifier: MIT
#ifndef LIBRA_EMULNK_H
#define LIBRA_EMULNK_H

#include "libra_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* EmuLnk companion protocol — lightweight UDP server.
 *
 * When enabled, libra listens on a UDP port and responds to
 * memory read requests from an EmuLnk companion app (Android,
 * web dashboard, etc.). This allows a second screen to display
 * live game data (HP, score, map, items) while playing.
 *
 * Protocol: EMLKV2 — clean-room implementation based on the
 * publicly documented message format. */

typedef struct libra_emulnk libra_emulnk_t;

/* Start the EmuLnk UDP server on the given port.
 * Returns NULL on failure. */
libra_emulnk_t *libra_emulnk_start(libra_ctx_t *ctx, int port);

/* Process pending UDP messages (call once per frame). */
void libra_emulnk_poll(libra_emulnk_t *lnk);

/* Stop the server and free resources. */
void libra_emulnk_stop(libra_emulnk_t *lnk);

#ifdef __cplusplus
}
#endif

#endif /* LIBRA_EMULNK_H */
