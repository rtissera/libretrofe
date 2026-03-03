// SPDX-License-Identifier: MIT
#ifndef LIBRA_NETPLAY_H
#define LIBRA_NETPLAY_H

#include "libra.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Forward declaration — full struct in netplay.c */
struct libra_netplay;

struct libra_netplay *libra_netplay_alloc(struct libra_ctx *ctx);
void                  libra_netplay_free(struct libra_netplay *np);

bool libra_np_host(struct libra_netplay *np, uint16_t port,
                   libra_net_message_cb_t msg_cb);
bool libra_np_join(struct libra_netplay *np, const char *host_ip,
                   uint16_t port, libra_net_message_cb_t msg_cb);
bool libra_np_poll(struct libra_netplay *np);
void libra_np_disconnect(struct libra_netplay *np);

bool     libra_np_active(const struct libra_netplay *np);
bool     libra_np_connected(const struct libra_netplay *np);
bool     libra_np_is_host(const struct libra_netplay *np);
bool     libra_np_is_waiting(const struct libra_netplay *np);
uint16_t libra_np_client_id(const struct libra_netplay *np);
unsigned libra_np_peer_count(const struct libra_netplay *np);

/* Spectate mode */
bool     libra_np_spectate(struct libra_netplay *np);
bool     libra_np_is_spectating(const struct libra_netplay *np);
unsigned libra_np_spectator_count(const struct libra_netplay *np);

/* Relay (MITM) */
bool libra_np_host_relay(struct libra_netplay *np,
                          const char *relay_ip, uint16_t relay_port,
                          uint8_t mitm_id_out[16],
                          libra_net_message_cb_t msg_cb);
bool libra_np_join_relay(struct libra_netplay *np,
                          const char *relay_ip, uint16_t relay_port,
                          const uint8_t mitm_id[16],
                          libra_net_message_cb_t msg_cb);

#endif /* LIBRA_NETPLAY_H */
