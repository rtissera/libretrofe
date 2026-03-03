// SPDX-License-Identifier: MIT
#ifndef LIBRA_ROLLBACK_H
#define LIBRA_ROLLBACK_H

#include "libra.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration — full struct in rollback.c */
struct libra_rollback;

struct libra_rollback *libra_rollback_alloc(struct libra_ctx *ctx);
void                   libra_rollback_free(struct libra_rollback *rb);

bool libra_rb_host(struct libra_rollback *rb, uint16_t port,
                   libra_net_message_cb_t msg_cb);
bool libra_rb_join(struct libra_rollback *rb, const char *host_ip,
                   uint16_t port, libra_net_message_cb_t msg_cb);

/* Call once per frame instead of libra_run().
 * Captures local input, exchanges with peer, handles rollback, runs core.
 * Returns false if connection lost. */
bool libra_rb_run(struct libra_rollback *rb);

void libra_rb_disconnect(struct libra_rollback *rb);

bool     libra_rb_active(const struct libra_rollback *rb);
bool     libra_rb_connected(const struct libra_rollback *rb);
bool     libra_rb_is_host(const struct libra_rollback *rb);
bool     libra_rb_is_replay(const struct libra_rollback *rb);

/* Input latency (additional frames of local input delay; default 0) */
void     libra_rb_set_input_latency(struct libra_rollback *rb, unsigned frames);
unsigned libra_rb_input_latency(const struct libra_rollback *rb);

/* Relay (MITM) */
bool libra_rb_host_relay(struct libra_rollback *rb,
                          const char *relay_ip, uint16_t relay_port,
                          uint8_t mitm_id_out[16],
                          libra_net_message_cb_t msg_cb);
bool libra_rb_join_relay(struct libra_rollback *rb,
                          const char *relay_ip, uint16_t relay_port,
                          const uint8_t mitm_id[16],
                          libra_net_message_cb_t msg_cb);

#endif /* LIBRA_ROLLBACK_H */
