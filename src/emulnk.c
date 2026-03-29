// SPDX-License-Identifier: MIT
//
// EmuLnk companion protocol — UDP server for live game data.
// Clean-room implementation of the EMLKV2 protocol.
//
// The companion app (running on a second screen) sends:
//   1. Handshake: "EMLKV2" → server responds with JSON identity
//   2. Memory read: offset + size → server responds with raw bytes
//
// This allows dashboards to display live game state (HP, score,
// inventory, map position) without modifying the emulator core.
//

#include "emulnk.h"
#include "netsock.h"
#include "libretro.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define EMULNK_MAGIC    "EMLKV2"
#define EMULNK_MAGIC_LEN 6

/* Message types (byte after magic in requests) */
#define MSG_HANDSHAKE   0x01
#define MSG_READ_MEM    0x02

/* Max UDP payload */
#define MAX_PAYLOAD     4096

struct libra_emulnk {
    libra_ctx_t    *ctx;
    libra_socket_t  sock;
    int             port;
    int             active;
};

libra_emulnk_t *libra_emulnk_start(libra_ctx_t *ctx, int port)
{
    if (!ctx) return NULL;

    libra_socket_t sock = libra_socket_udp();
    if (sock == LIBRA_INVALID_SOCKET)
        return NULL;

    if (!libra_socket_bind(sock, port)) {
        libra_socket_close(sock);
        return NULL;
    }

    libra_socket_set_nonblocking_udp(sock);

    libra_emulnk_t *lnk = calloc(1, sizeof(*lnk));
    if (!lnk) { libra_socket_close(sock); return NULL; }

    lnk->ctx    = ctx;
    lnk->sock   = sock;
    lnk->port   = port;
    lnk->active = 1;

    return lnk;
}

/* Build JSON identity response for handshake */
static int build_identity(libra_ctx_t *ctx, char *buf, int bufsize)
{
    const char *core_name = libra_core_name(ctx);

    return snprintf(buf, bufsize,
        "{"
        "\"protocol\":\"EMLKV2\","
        "\"app\":\"libra\","
        "\"core\":\"%s\","
        "\"game\":\"\","
        "\"platform\":\"\","
        "\"memory_size\":%u"
        "}",
        core_name ? core_name : "",
        (unsigned)libra_get_memory_size(ctx, RETRO_MEMORY_SYSTEM_RAM));
}

void libra_emulnk_poll(libra_emulnk_t *lnk)
{
    if (!lnk || !lnk->active)
        return;

    unsigned char recv_buf[MAX_PAYLOAD];
    char from[128]; /* sockaddr storage, opaque */
    unsigned int fromlen = sizeof(from);

    for (;;) {
        int n = libra_socket_recvfrom(lnk->sock, recv_buf, sizeof(recv_buf),
                                       from, &fromlen);
        if (n <= 0)
            break; /* no more pending datagrams */

        /* Must start with EMLKV2 magic */
        if (n < EMULNK_MAGIC_LEN || memcmp(recv_buf, EMULNK_MAGIC, EMULNK_MAGIC_LEN) != 0)
            continue;

        if (n < EMULNK_MAGIC_LEN + 1)
            continue;

        unsigned char msg_type = recv_buf[EMULNK_MAGIC_LEN];

        if (msg_type == MSG_HANDSHAKE) {
            /* Respond with JSON identity */
            char resp[1024];
            int len = build_identity(lnk->ctx, resp, sizeof(resp));
            if (len > 0)
                libra_socket_sendto(lnk->sock, resp, len, from, fromlen);
        }
        else if (msg_type == MSG_READ_MEM) {
            /* Format: EMLKV2 | 0x02 | memtype(1) | offset(4 LE) | size(2 LE) */
            if (n < EMULNK_MAGIC_LEN + 1 + 1 + 4 + 2)
                continue;

            unsigned char *p = recv_buf + EMULNK_MAGIC_LEN + 1;
            unsigned memtype = p[0];
            unsigned offset  = (unsigned)p[1] | ((unsigned)p[2] << 8) |
                               ((unsigned)p[3] << 16) | ((unsigned)p[4] << 24);
            unsigned size    = (unsigned)p[5] | ((unsigned)p[6] << 8);

            if (size > MAX_PAYLOAD - 8)
                size = MAX_PAYLOAD - 8;

            void *mem = libra_get_memory_data(lnk->ctx, memtype);
            unsigned mem_size = (unsigned)libra_get_memory_size(lnk->ctx, memtype);

            if (!mem || offset + size > mem_size) {
                /* Send error: empty response */
                unsigned char err = 0xFF;
                libra_socket_sendto(lnk->sock, &err, 1, from, fromlen);
                continue;
            }

            /* Send raw memory bytes */
            libra_socket_sendto(lnk->sock, (const char*)mem + offset, size,
                                from, fromlen);
        }
    }
}

void libra_emulnk_stop(libra_emulnk_t *lnk)
{
    if (!lnk) return;
    lnk->active = 0;
    if (lnk->sock != LIBRA_INVALID_SOCKET)
        libra_socket_close(lnk->sock);
    free(lnk);
}
