// SPDX-License-Identifier: MIT
#ifndef LIBRA_NETSOCK_H
#define LIBRA_NETSOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Wire-protocol constants (interop values, not copied code)
 * ---------------------------------------------------------------------- */

#define NP_MAGIC           0x52414E50u  /* "RANP" */
#define NP_NICK_LEN        32
#define NP_MAX_DEVICES      16
#define NP_DEFAULT_PORT    55435
#define NP_PROTO_LO         5
#define NP_PROTO_HI         7   /* protocol 7 = NETPLAY1 container */
#define NP_RECV_BUF_SIZE   (128 * 1024)
#define NP_MAX_PKT_DATA    65536
#define NP_MAX_PEERS        15

/* Command IDs (netpacket mode) */
#define CMD_DISCONNECT      0x0002u
#define CMD_NICK            0x0020u
#define CMD_INFO            0x0022u
#define CMD_SYNC            0x0023u
#define CMD_SPECTATE        0x0024u
#define CMD_PLAY            0x0025u
#define CMD_MODE            0x0026u
#define CMD_MODE_REFUSED    0x0027u
#define CMD_NETPACKET       0x0048u
#define CMD_PING_REQUEST    0x1100u
#define CMD_PING_RESPONSE   0x1101u

/* Command IDs (rollback / input-frame-sync mode) */
#define CMD_INPUT           0x0003u
#define CMD_NOINPUT         0x0004u
#define CMD_CRC             0x0040u
#define CMD_REQUEST_SAVESTATE 0x0041u
#define CMD_LOAD_SAVESTATE       0x0042u
#define CMD_LOAD_SAVESTATE_DELTA 0x0043u  /* payload: frame(4)+orig_size(4)+crc(4)+XOR-delta compressed */
#define CMD_STALL                0x0045u

/* MODE bits */
#define MODE_BIT_YOU        (1u << 31)
#define MODE_BIT_PLAYING    (1u << 30)

/* MITM relay constants (interop values) */
#define MITM_LINK_MAGIC    0x5241544Cu  /* "RATL" — host creates session */
#define MITM_ADDR_MAGIC    0x52415441u  /* "RATA" — client joins session */
#define MITM_MAGIC         0x52415453u  /* "RATS" — session ID magic prefix */
#define MITM_ID_SIZE       16           /* 4-byte magic + 12-byte unique */

/* -------------------------------------------------------------------------
 * Shared TCP helpers
 * ---------------------------------------------------------------------- */

bool netsock_set_nonblocking(int fd);
bool netsock_set_nodelay(int fd);

/* Blocking write of exactly len bytes. Returns false on error. */
bool netsock_write_all(int fd, const void *buf, size_t len);

/* Blocking read of exactly len bytes with a timeout (ms).
 * Returns false on error or timeout. */
bool netsock_read_all(int fd, void *buf, size_t len, int timeout_ms);

/* Write a command (cmd_id + payload_len header + data) */
bool netsock_write_cmd(int fd, uint32_t cmd_id,
                       const void *data, uint32_t data_len);

/* Platform / implementation magic */
uint32_t netsock_platform_magic(void);
uint32_t netsock_impl_magic(void);

/* -------------------------------------------------------------------------
 * IPv4 + IPv6 dual-stack helpers
 * ---------------------------------------------------------------------- */

/* Connect to host:port using getaddrinfo (supports IPv4 and IPv6 literals
 * and hostnames).  Non-blocking connect with 5-second timeout.
 * Returns a connected, blocking, TCP_NODELAY file descriptor, or -1. */
int netsock_tcp_connect(const char *host, uint16_t port);

/* Create a dual-stack (AF_INET6 + IPV6_V6ONLY=0) TCP listening socket bound
 * to port on all interfaces.  Falls back to AF_INET if IPv6 is unavailable.
 * Returns a blocking fd ready for accept() (caller should set non-blocking),
 * or -1 on failure. */
int netsock_tcp_listen(uint16_t port);

#endif /* LIBRA_NETSOCK_H */
