// SPDX-License-Identifier: MIT
/*
 * libra netplay — RetroArch-compatible netpacket transport
 *
 * Clean-room implementation of the netplay wire protocol for the
 * CORE_PACKET_INTERFACE mode.  Interoperable with RetroArch; no GPL
 * code was copied.  Only POSIX sockets and the MIT-licensed libretro.h
 * API header are used.
 */
#include "netplay.h"
#include "netsock.h"
#include "libra_internal.h"
#include "environment.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

/* Local aliases for shared helpers (keep existing call sites short) */
#define set_nonblocking  netsock_set_nonblocking
#define set_nodelay      netsock_set_nodelay
#define write_all        netsock_write_all
#define read_all         netsock_read_all
#define write_cmd        netsock_write_cmd
#define libra_platform_magic netsock_platform_magic
#define libra_impl_magic     netsock_impl_magic

/* -------------------------------------------------------------------------
 * Per-peer state
 * ---------------------------------------------------------------------- */

struct np_peer {
    int       fd;
    uint16_t  client_id;
    bool      playing;
    bool      spectating;    /* observer only, receives but does not send */
    char      nick[NP_NICK_LEN];
    uint8_t   recv_buf[NP_RECV_BUF_SIZE];
    size_t    recv_len;
};

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

struct libra_netplay {
    libra_ctx_t           *ctx;
    int                    listen_fd;
    bool                   is_host;
    bool                   core_started;
    uint16_t               client_id;       /* our id (0=host, >=1 client) */
    uint16_t               next_client_id;  /* host: next id to assign */
    uint16_t               port;
    libra_net_message_cb_t msg_cb;
    void                  *msg_ud;

    /* Host: peers[0..peer_count-1] = connected clients (heap-allocated)
     * Client: peers[0] = host connection */
    struct np_peer *peers[NP_MAX_PEERS];
    int             peer_count;

    char our_nick[NP_NICK_LEN];
    bool spectating;   /* we joined as spectator */
};

/* Global pointer for trampolines (only one netplay session at a time) */
static struct libra_netplay *s_np = NULL;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void np_msg(struct libra_netplay *np, const char *msg)
{
    fprintf(stderr, "libra [NET] %s\n", msg);
    if (np->msg_cb)
        np->msg_cb(np->msg_ud, msg);
}

static void np_msgf(struct libra_netplay *np, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void np_msgf(struct libra_netplay *np, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    np_msg(np, buf);
}

/* Helper functions (set_nonblocking, set_nodelay, write_all, read_all,
 * write_cmd, libra_platform_magic, libra_impl_magic) are now provided
 * by netsock.c via the #define aliases above. */

/* -------------------------------------------------------------------------
 * Core trampolines (called by the core via function pointers)
 * ---------------------------------------------------------------------- */

static void RETRO_CALLCONV np_send_trampoline(
    int flags, const void *buf, size_t len, uint16_t client_id)
{
    (void)flags; /* TCP is always reliable; flags are advisory */
    struct libra_netplay *np = s_np;
    if (!np || np->peer_count == 0) return;
    if (!buf || len == 0) {
        /* Flush hint with no data — just return, TCP auto-flushes with
         * TCP_NODELAY already set. */
        return;
    }
    if (len > NP_MAX_PKT_DATA) return;

    /* NETPACKET wire format:
     *   cmd(4) + payload_len(4) + pkt_client_id(4) + data(payload_len)
     * payload_len = len of actual data (not including pkt_client_id). */
    uint32_t hdr[3];
    hdr[0] = htonl(CMD_NETPACKET);
    hdr[1] = htonl((uint32_t)len);

    if (!np->is_host) {
        /* Client → Host: pkt_client_id = target (or broadcast) */
        hdr[2] = htonl((uint32_t)client_id);
        if (np->peers[0] && np->peers[0]->fd >= 0) {
            write_all(np->peers[0]->fd, hdr, 12);
            write_all(np->peers[0]->fd, buf, len);
        }
    } else {
        /* Host → Client(s): pkt_client_id = source (us = 0) */
        hdr[2] = htonl(0);
        if (client_id == 0xFFFF) {
            /* Broadcast to ALL playing + spectating peers */
            for (int i = 0; i < np->peer_count; i++) {
                if (np->peers[i] && (np->peers[i]->playing ||
                                      np->peers[i]->spectating)) {
                    write_all(np->peers[i]->fd, hdr, 12);
                    write_all(np->peers[i]->fd, buf, len);
                }
            }
        } else {
            /* Unicast to specific peer */
            for (int i = 0; i < np->peer_count; i++) {
                if (np->peers[i] && np->peers[i]->client_id == client_id) {
                    write_all(np->peers[i]->fd, hdr, 12);
                    write_all(np->peers[i]->fd, buf, len);
                    break;
                }
            }
        }
    }
}

static void RETRO_CALLCONV np_poll_receive_trampoline(void)
{
    struct libra_netplay *np = s_np;
    if (!np) return;
    libra_np_poll(np);
}

/* -------------------------------------------------------------------------
 * Handshake helpers
 * ---------------------------------------------------------------------- */

/* Send the 6-word initial header */
static bool send_header(struct libra_netplay *np, int fd, uint32_t proto)
{
    uint32_t hdr[6];
    hdr[0] = htonl(NP_MAGIC);
    hdr[1] = htonl(libra_platform_magic());
    hdr[2] = htonl(0); /* compression: none */

    if (np->is_host) {
        hdr[3] = htonl(0);     /* no password (salt=0) */
    } else {
        hdr[3] = htonl(NP_PROTO_HI); /* client sends high proto in salt field */
    }

    hdr[4] = htonl(proto);
    hdr[5] = htonl(libra_impl_magic());
    return write_all(fd, hdr, sizeof(hdr));
}

/* Receive and validate the 6-word header. Stores negotiated protocol. */
static bool recv_header(struct libra_netplay *np, int fd, uint32_t *out_proto)
{
    uint32_t hdr[6];
    if (!read_all(fd, hdr, sizeof(hdr), 5000))
        return false;

    uint32_t magic = ntohl(hdr[0]);
    if (magic != NP_MAGIC) {
        np_msg(np, "Not a compatible netplay peer");
        return false;
    }

    uint32_t lo_proto = ntohl(hdr[4]);
    uint32_t hi_proto = ntohl(hdr[3]); /* client puts high in salt field */

    if (np->is_host) {
        /* We're server: negotiate protocol */
        uint32_t best = hi_proto ? hi_proto : lo_proto;
        if (best > NP_PROTO_HI) best = NP_PROTO_HI;
        if (best < NP_PROTO_LO) {
            np_msg(np, "Peer protocol version too old");
            return false;
        }
        *out_proto = best;
    } else {
        /* We're client: server told us the negotiated protocol */
        *out_proto = lo_proto;
        if (*out_proto < NP_PROTO_LO || *out_proto > NP_PROTO_HI) {
            np_msg(np, "Server protocol version incompatible");
            return false;
        }
    }

    return true;
}

/* Send NICK command */
static bool send_nick(struct libra_netplay *np, int fd)
{
    uint8_t payload[NP_NICK_LEN];
    memset(payload, 0, sizeof(payload));
    strncpy((char *)payload, np->our_nick, NP_NICK_LEN - 1);
    return write_cmd(fd, CMD_NICK, payload, NP_NICK_LEN);
}

/* Receive NICK command */
static bool recv_nick(struct libra_netplay *np, int fd, char *nick_out)
{
    uint32_t hdr[2];
    if (!read_all(fd, hdr, 8, 5000)) return false;
    if (ntohl(hdr[0]) != CMD_NICK || ntohl(hdr[1]) != NP_NICK_LEN) {
        np_msg(np, "Expected NICK command");
        return false;
    }
    if (!read_all(fd, nick_out, NP_NICK_LEN, 5000))
        return false;
    nick_out[NP_NICK_LEN - 1] = '\0';
    return true;
}

/* Build and send INFO command */
static bool send_info(struct libra_netplay *np, int fd)
{
    /* INFO payload: content_crc(4) + core_name(32) + core_version(32) = 68 */
    uint8_t payload[68];
    memset(payload, 0, sizeof(payload));

    /* content_crc = 0 (we don't compute it; mismatch is a warning) */
    uint32_t crc = htonl(0);
    memcpy(payload, &crc, 4);

    /* core_name */
    const char *name = np->ctx->core ? np->ctx->core->sys_info.library_name : "unknown";
    if (name) strncpy((char *)payload + 4, name, NP_NICK_LEN - 1);

    /* core_version — use protocol_version if the core set one */
    const char *ver = NULL;
    if (np->ctx->has_netpacket && np->ctx->netpacket_cb.protocol_version)
        ver = np->ctx->netpacket_cb.protocol_version;
    else if (np->ctx->core)
        ver = np->ctx->core->sys_info.library_version;
    if (ver) strncpy((char *)payload + 36, ver, NP_NICK_LEN - 1);

    return write_cmd(fd, CMD_INFO, payload, sizeof(payload));
}

/* Receive and validate INFO command */
static bool recv_info(struct libra_netplay *np, int fd)
{
    uint32_t hdr[2];
    if (!read_all(fd, hdr, 8, 5000)) return false;
    if (ntohl(hdr[0]) != CMD_INFO) {
        np_msg(np, "Expected INFO command");
        return false;
    }

    uint32_t sz = ntohl(hdr[1]);
    if (sz < 68) {
        np_msg(np, "INFO payload too small");
        return false;
    }

    uint8_t payload[68];
    if (!read_all(fd, payload, 68, 5000)) return false;

    /* Skip any extra bytes beyond what we understand */
    if (sz > 68) {
        uint8_t skip[256];
        uint32_t remaining = sz - 68;
        while (remaining > 0) {
            uint32_t chunk = remaining > sizeof(skip) ? sizeof(skip) : remaining;
            if (!read_all(fd, skip, chunk, 5000)) return false;
            remaining -= chunk;
        }
    }

    /* Validate core_name */
    char peer_core[NP_NICK_LEN];
    memcpy(peer_core, payload + 4, NP_NICK_LEN);
    peer_core[NP_NICK_LEN - 1] = '\0';

    const char *our_core = np->ctx->core
        ? np->ctx->core->sys_info.library_name : "unknown";

    if (our_core && peer_core[0] && strcasecmp(peer_core, our_core) != 0) {
        np_msgf(np, "Incompatible core: %s vs %s", peer_core, our_core);
        return false;
    }

    return true;
}

/* Host: send SYNC command (simplified for netpacket mode) */
static bool send_sync(struct libra_netplay *np, int fd,
                       uint32_t client_num, const char *nick)
{
    (void)np;
    /* SYNC payload:
     *   frame_count(4) + client_num(4) +
     *   devices[16](64) + share_modes[16](16) +
     *   device_clients[16](64) + nick(32) = 184 bytes
     *   No SRAM in netpacket mode. */
    uint8_t payload[184];
    memset(payload, 0, sizeof(payload));
    size_t off = 0;

    /* frame_count = 0 */
    uint32_t val = htonl(0);
    memcpy(payload + off, &val, 4); off += 4;

    /* client_num */
    val = htonl(client_num);
    memcpy(payload + off, &val, 4); off += 4;

    /* config_devices[16]: port 0 = RETRO_DEVICE_JOYPAD (1), rest = NONE (0) */
    val = htonl(1); /* RETRO_DEVICE_JOYPAD */
    memcpy(payload + off, &val, 4);
    off += NP_MAX_DEVICES * 4;

    /* share_modes[16]: all 0 */
    off += NP_MAX_DEVICES;

    /* device_clients[16]: all 0 (netpacket mode skips device assignment) */
    off += NP_MAX_DEVICES * 4;

    /* nick (the assigned nick for the client) */
    strncpy((char *)payload + off, nick, NP_NICK_LEN - 1);

    return write_cmd(fd, CMD_SYNC, payload, sizeof(payload));
}

/* Client: receive SYNC command */
static bool recv_sync(struct libra_netplay *np, int fd)
{
    uint32_t hdr[2];
    if (!read_all(fd, hdr, 8, 5000)) return false;
    if (ntohl(hdr[0]) != CMD_SYNC) {
        np_msg(np, "Expected SYNC command");
        return false;
    }

    uint32_t sz = ntohl(hdr[1]);
    /* Minimum size: frame(4) + client_num(4) + devices(64) + share(16) +
     * dev_clients(64) + nick(32) = 184 */
    if (sz < 184) {
        np_msg(np, "SYNC payload too small");
        return false;
    }

    /* Read frame_count + client_num */
    uint32_t fc, cn;
    if (!read_all(fd, &fc, 4, 5000)) return false;
    if (!read_all(fd, &cn, 4, 5000)) return false;

    np->client_id = (uint16_t)(ntohl(cn) & 0x7FFFFFFF);

    /* Skip devices(64) + share_modes(16) + device_clients(64) + nick(32) = 176 */
    uint32_t skip_len = sz - 8;
    uint8_t skip[256];
    while (skip_len > 0) {
        uint32_t chunk = skip_len > sizeof(skip) ? sizeof(skip) : skip_len;
        if (!read_all(fd, skip, chunk, 5000)) return false;
        skip_len -= chunk;
    }

    return true;
}

/* Client: send PLAY command (request to become a playing client) */
static bool send_play(struct libra_netplay *np, int fd)
{
    (void)np;
    /* PLAY payload: uint32_t mode. For netpacket: devices=0, share=0 */
    uint32_t mode = htonl(0);
    return write_cmd(fd, CMD_PLAY, &mode, sizeof(mode));
}

/* Host: send MODE command to accept a player */
static bool send_mode_accepted(struct libra_netplay *np, int fd,
                                uint32_t client_num, const char *nick)
{
    (void)np;
    /* MODE payload: frame(4) + mode(4) + devices(4) + share_modes(16) + nick(32) = 60 */
    uint8_t payload[60];
    memset(payload, 0, sizeof(payload));

    /* frame = 0 */
    uint32_t val = htonl(0);
    memcpy(payload, &val, 4);

    /* mode bits: YOU | PLAYING | client_num */
    val = htonl(MODE_BIT_YOU | MODE_BIT_PLAYING | client_num);
    memcpy(payload + 4, &val, 4);

    /* devices = 0 (netpacket mode) */
    /* share_modes: all 0 */
    /* nick */
    strncpy((char *)payload + 28, nick, NP_NICK_LEN - 1);

    return write_cmd(fd, CMD_MODE, payload, sizeof(payload));
}

/* Client: receive MODE (or MODE_REFUSED) */
static bool recv_mode(struct libra_netplay *np, int fd)
{
    uint32_t hdr[2];
    if (!read_all(fd, hdr, 8, 5000)) return false;

    uint32_t cmd = ntohl(hdr[0]);
    uint32_t sz  = ntohl(hdr[1]);

    if (cmd == CMD_MODE_REFUSED) {
        /* Skip payload */
        uint8_t skip[64];
        while (sz > 0) {
            uint32_t chunk = sz > sizeof(skip) ? sizeof(skip) : sz;
            if (!read_all(fd, skip, chunk, 5000)) return false;
            sz -= chunk;
        }
        np_msg(np, "Server refused play request");
        return false;
    }

    if (cmd != CMD_MODE || sz < 60) {
        np_msg(np, "Expected MODE command");
        return false;
    }

    /* Read and verify we got PLAYING */
    uint8_t payload[60];
    if (!read_all(fd, payload, 60, 5000)) return false;

    /* Skip any extra */
    if (sz > 60) {
        uint8_t skip[64];
        uint32_t remaining = sz - 60;
        while (remaining > 0) {
            uint32_t chunk = remaining > sizeof(skip) ? sizeof(skip) : remaining;
            if (!read_all(fd, skip, chunk, 5000)) return false;
            remaining -= chunk;
        }
    }

    uint32_t mode_bits;
    memcpy(&mode_bits, payload + 4, 4);
    mode_bits = ntohl(mode_bits);

    /* If we asked to play but didn't get PLAYING, fail.
     * If we asked to spectate, PLAYING clear is expected. */
    if (!np->spectating && !(mode_bits & MODE_BIT_PLAYING)) {
        np_msg(np, "Server did not grant playing mode");
        return false;
    }

    return true;
}

/* Client: send SPECTATE command */
static bool send_spectate(struct libra_netplay *np, int fd)
{
    (void)np;
    return write_cmd(fd, CMD_SPECTATE, NULL, 0);
}

/* Host: send MODE command for a spectator (YOU set, PLAYING clear) */
static bool send_mode_spectator(struct libra_netplay *np, int fd,
                                 uint32_t client_num, const char *nick)
{
    (void)np;
    uint8_t payload[60];
    memset(payload, 0, sizeof(payload));

    uint32_t val = htonl(0);
    memcpy(payload, &val, 4);

    /* mode bits: YOU set, PLAYING clear, client_num */
    val = htonl(MODE_BIT_YOU | client_num);
    memcpy(payload + 4, &val, 4);

    strncpy((char *)payload + 28, nick, NP_NICK_LEN - 1);
    return write_cmd(fd, CMD_MODE, payload, sizeof(payload));
}

/* Host: receive PLAY or SPECTATE command from client.
 * Sets *out_spectate = true if client sent SPECTATE. */
static bool recv_play_or_spectate(struct libra_netplay *np, int fd,
                                   bool *out_spectate)
{
    *out_spectate = false;
    uint32_t hdr[2];
    if (!read_all(fd, hdr, 8, 5000)) return false;

    uint32_t cmd = ntohl(hdr[0]);
    uint32_t sz  = ntohl(hdr[1]);

    if (cmd != CMD_PLAY && cmd != CMD_SPECTATE) {
        /* Could be something else — skip and retry once */
        uint8_t skip[256];
        while (sz > 0) {
            uint32_t chunk = sz > sizeof(skip) ? sizeof(skip) : sz;
            if (!read_all(fd, skip, chunk, 5000)) return false;
            sz -= chunk;
        }
        /* Try reading one more command */
        if (!read_all(fd, hdr, 8, 5000)) return false;
        cmd = ntohl(hdr[0]);
        sz  = ntohl(hdr[1]);
        if (cmd != CMD_PLAY && cmd != CMD_SPECTATE) {
            np_msg(np, "Expected PLAY or SPECTATE command from client");
            return false;
        }
    }

    if (cmd == CMD_SPECTATE)
        *out_spectate = true;

    /* Skip payload */
    if (sz > 0) {
        uint8_t skip[64];
        while (sz > 0) {
            uint32_t chunk = sz > sizeof(skip) ? sizeof(skip) : sz;
            if (!read_all(fd, skip, chunk, 5000)) return false;
            sz -= chunk;
        }
    }

    return true;
}

/* -------------------------------------------------------------------------
 * Relay fd helpers
 * ---------------------------------------------------------------------- */

/* Connect to a relay server and return a TCP fd, or -1 on failure. */
static int np_relay_connect(struct libra_netplay *np,
                             const char *ip, uint16_t port)
{
    int fd = netsock_tcp_connect(ip, port);
    if (fd < 0) np_msg(np, "Relay connection failed");
    return fd;
}

/* Host: connect to relay, send MITM_LINK_MAGIC, receive mitm_id.
 * Returns connected fd, or -1 on failure. */
static int np_relay_create_session(struct libra_netplay *np,
                                    const char *ip, uint16_t port,
                                    uint8_t mitm_id_out[MITM_ID_SIZE])
{
    int fd = np_relay_connect(np, ip, port);
    if (fd < 0) return -1;

    uint32_t magic = htonl(MITM_LINK_MAGIC);
    if (!write_all(fd, &magic, 4)) {
        np_msg(np, "Relay handshake failed (send LINK)");
        close(fd); return -1;
    }

    if (!read_all(fd, mitm_id_out, MITM_ID_SIZE, 5000)) {
        np_msg(np, "Relay handshake failed (recv session ID)");
        close(fd); return -1;
    }

    return fd;
}

/* Client: connect to relay with known mitm_id.
 * Returns connected fd, or -1 on failure. */
static int np_relay_join_session(struct libra_netplay *np,
                                  const char *ip, uint16_t port,
                                  const uint8_t mitm_id[MITM_ID_SIZE])
{
    int fd = np_relay_connect(np, ip, port);
    if (fd < 0) return -1;

    uint32_t magic = htonl(MITM_ADDR_MAGIC);
    if (!write_all(fd, &magic, 4)) {
        np_msg(np, "Relay handshake failed (send ADDR)");
        close(fd); return -1;
    }

    if (!write_all(fd, mitm_id, MITM_ID_SIZE)) {
        np_msg(np, "Relay handshake failed (send session ID)");
        close(fd); return -1;
    }

    /* Read echo of session ID as confirmation */
    uint8_t echo[MITM_ID_SIZE];
    if (!read_all(fd, echo, MITM_ID_SIZE, 5000)) {
        np_msg(np, "Relay handshake failed (recv echo)");
        close(fd); return -1;
    }

    return fd;
}

/* -------------------------------------------------------------------------
 * Start/stop core netpacket session
 * ---------------------------------------------------------------------- */

static void core_start(struct libra_netplay *np)
{
    if (np->core_started) return;
    if (!np->ctx->has_netpacket || !np->ctx->netpacket_cb.start) return;

    s_np = np;
    libra_environment_set_ctx(np->ctx);
    np->ctx->netpacket_cb.start(
        np->client_id, np_send_trampoline, np_poll_receive_trampoline);
    np->core_started = true;
}

static void core_stop(struct libra_netplay *np)
{
    if (!np->core_started) return;
    s_np = np;
    libra_environment_set_ctx(np->ctx);
    if (np->ctx->netpacket_cb.stop)
        np->ctx->netpacket_cb.stop();
    np->core_started = false;
}

static bool core_connected(struct libra_netplay *np, uint16_t cid)
{
    if (!np->ctx->has_netpacket) return true;
    if (!np->ctx->netpacket_cb.connected) return true;
    s_np = np;
    libra_environment_set_ctx(np->ctx);
    return np->ctx->netpacket_cb.connected(cid);
}

static void core_disconnected(struct libra_netplay *np, uint16_t cid)
{
    if (!np->ctx->has_netpacket) return;
    if (!np->ctx->netpacket_cb.disconnected) return;
    s_np = np;
    libra_environment_set_ctx(np->ctx);
    np->ctx->netpacket_cb.disconnected(cid);
}

/* -------------------------------------------------------------------------
 * Relay helper (host relays a packet from one peer to another)
 * ---------------------------------------------------------------------- */

static void relay_packet(int dest_fd, uint16_t source,
                          const void *data, size_t len)
{
    uint32_t hdr[3];
    hdr[0] = htonl(CMD_NETPACKET);
    hdr[1] = htonl((uint32_t)len);
    hdr[2] = htonl((uint32_t)source);
    write_all(dest_fd, hdr, 12);
    if (len > 0)
        write_all(dest_fd, data, len);
}

/* -------------------------------------------------------------------------
 * Command dispatch (during active session)
 * ---------------------------------------------------------------------- */

/* Process one complete command from peer's recv_buf at offset *pos.
 * Returns false if we need more data. */
static bool dispatch_cmd(struct libra_netplay *np,
                          struct np_peer *peer, size_t *pos)
{
    size_t avail = peer->recv_len - *pos;
    if (avail < 8) return false;  /* need cmd header */

    uint32_t raw_cmd, raw_sz;
    memcpy(&raw_cmd, peer->recv_buf + *pos, 4);
    memcpy(&raw_sz,  peer->recv_buf + *pos + 4, 4);
    uint32_t cmd  = ntohl(raw_cmd);
    uint32_t sz   = ntohl(raw_sz);

    /* For NETPACKET: we need sz bytes of data + 4 bytes of pkt_client_id */
    if (cmd == CMD_NETPACKET) {
        if (avail < 8 + 4 + sz) return false;  /* incomplete */
        *pos += 8;

        uint32_t raw_cid;
        memcpy(&raw_cid, peer->recv_buf + *pos, 4);
        uint16_t pkt_cid = (uint16_t)ntohl(raw_cid);
        *pos += 4;

        /* Dispatch to core */
        if (np->ctx->has_netpacket && np->ctx->netpacket_cb.receive) {
            s_np = np;
            libra_environment_set_ctx(np->ctx);

            if (!np->is_host) {
                /* Client: pkt_cid = sender */
                np->ctx->netpacket_cb.receive(
                    peer->recv_buf + *pos, sz, pkt_cid);
            } else {
                /* Host: pkt_cid = target (0=host, broadcast, or specific) */
                uint16_t sender = peer->client_id;
                bool is_broadcast = (pkt_cid == 0xFFFF);

                /* Deliver to core if targeted at host or broadcast */
                if (is_broadcast || pkt_cid == 0) {
                    np->ctx->netpacket_cb.receive(
                        peer->recv_buf + *pos, sz, sender);
                }

                /* Relay to other peers (including spectators) */
                if (is_broadcast) {
                    for (int i = 0; i < np->peer_count; i++) {
                        if (np->peers[i] && np->peers[i] != peer &&
                            (np->peers[i]->playing ||
                             np->peers[i]->spectating)) {
                            relay_packet(np->peers[i]->fd, sender,
                                         peer->recv_buf + *pos, sz);
                        }
                    }
                } else if (pkt_cid != 0) {
                    /* Unicast to specific peer */
                    for (int i = 0; i < np->peer_count; i++) {
                        if (np->peers[i] &&
                            np->peers[i]->client_id == pkt_cid) {
                            relay_packet(np->peers[i]->fd, sender,
                                         peer->recv_buf + *pos, sz);
                            break;
                        }
                    }
                }
            }
        }
        *pos += sz;
        return true;
    }

    /* All other commands: need the full payload */
    if (avail < 8 + sz) return false;
    *pos += 8;

    switch (cmd) {
        case CMD_DISCONNECT:
            *pos += sz;
            /* Signal disconnect — will be cleaned up in poll */
            if (peer->fd >= 0) {
                close(peer->fd);
                peer->fd = -1;
            }
            return true;

        case CMD_PING_REQUEST: {
            /* Respond with PING_RESPONSE (empty payload) */
            *pos += sz;
            if (peer->fd >= 0)
                write_cmd(peer->fd, CMD_PING_RESPONSE, NULL, 0);
            return true;
        }

        case CMD_PING_RESPONSE:
            *pos += sz;
            return true;

        default:
            /* Unknown command — skip payload */
            *pos += sz;
            return true;
    }
}

/* Read pending TCP data from a single peer and dispatch commands */
static void read_and_dispatch_peer(struct libra_netplay *np,
                                    struct np_peer *peer)
{
    if (peer->fd < 0) return;

    /* Non-blocking read into peer's recv_buf */
    size_t space = NP_RECV_BUF_SIZE - peer->recv_len;
    if (space > 0) {
        ssize_t n = recv(peer->fd, peer->recv_buf + peer->recv_len, space, 0);
        if (n > 0) {
            peer->recv_len += (size_t)n;
        } else if (n == 0) {
            /* Connection closed */
            close(peer->fd);
            peer->fd = -1;
            return;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(peer->fd);
                peer->fd = -1;
                return;
            }
        }
    }

    /* Dispatch as many complete commands as possible */
    size_t pos = 0;
    while (dispatch_cmd(np, peer, &pos))
        ;

    /* Compact the buffer */
    if (pos > 0 && pos < peer->recv_len) {
        memmove(peer->recv_buf, peer->recv_buf + pos, peer->recv_len - pos);
        peer->recv_len -= pos;
    } else if (pos >= peer->recv_len) {
        peer->recv_len = 0;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

struct libra_netplay *libra_netplay_alloc(struct libra_ctx *ctx)
{
    struct libra_netplay *np = calloc(1, sizeof(*np));
    if (!np) return NULL;
    np->ctx       = ctx;
    np->listen_fd = -1;
    np->msg_ud    = ctx->config.userdata;
    strncpy(np->our_nick, "libra", NP_NICK_LEN - 1);
    return np;
}

void libra_netplay_free(struct libra_netplay *np)
{
    if (!np) return;
    libra_np_disconnect(np);
    if (s_np == np) s_np = NULL;
    free(np);
}

bool libra_np_host(struct libra_netplay *np, uint16_t port,
                   libra_net_message_cb_t msg_cb)
{
    if (!np || !np->ctx->has_netpacket) return false;
    libra_np_disconnect(np);

    np->msg_cb = msg_cb;
    np->is_host = true;
    np->client_id = 0;
    np->next_client_id = 1;
    np->port = port ? port : NP_DEFAULT_PORT;

    /* Create listening socket (dual-stack IPv4+IPv6) */
    int fd = netsock_tcp_listen(np->port);
    if (fd < 0) {
        np_msgf(np, "Failed to bind port %u", np->port);
        return false;
    }

    set_nonblocking(fd);
    np->listen_fd = fd;

    /* Start the core's netpacket session as host (client_id=0) */
    core_start(np);

    np_msgf(np, "Hosting on port %u...", np->port);
    return true;
}

bool libra_np_join(struct libra_netplay *np, const char *host_ip,
                   uint16_t port, libra_net_message_cb_t msg_cb)
{
    if (!np || !np->ctx->has_netpacket || !host_ip) return false;
    libra_np_disconnect(np);

    np->msg_cb = msg_cb;
    np->is_host = false;
    np->port = port ? port : NP_DEFAULT_PORT;

    np_msgf(np, "Connecting to %s:%u...", host_ip, np->port);

    int fd = netsock_tcp_connect(host_ip, np->port);
    if (fd < 0) {
        np_msg(np, "Connection failed");
        return false;
    }

    /* Create peer struct for host connection */
    struct np_peer *peer = calloc(1, sizeof(*peer));
    if (!peer) {
        close(fd);
        return false;
    }
    peer->fd = fd;
    peer->client_id = 0; /* host is always client_id 0 */
    np->peers[0] = peer;
    np->peer_count = 1;

    /* --- Client handshake ---
     * 1. Send our header (with proto=LOW, high proto in salt field)
     * 2. Receive server header
     * 3. Send NICK
     * 4. Receive server NICK
     * 5. Receive server INFO
     * 6. Send our INFO
     * 7. Receive SYNC
     * 8. Start core netpacket
     * 9. Send PLAY
     * 10. Receive MODE
     */

    /* 1. Send header */
    if (!send_header(np, fd, NP_PROTO_LO)) {
        np_msg(np, "Handshake failed (send header)");
        goto fail;
    }

    /* 2. Receive server header */
    uint32_t proto;
    if (!recv_header(np, fd, &proto)) goto fail;

    /* 3. Send NICK */
    if (!send_nick(np, fd)) {
        np_msg(np, "Handshake failed (send nick)");
        goto fail;
    }

    /* 4. Receive NICK */
    if (!recv_nick(np, fd, peer->nick)) goto fail;

    /* 5. Receive INFO */
    if (!recv_info(np, fd)) goto fail;

    /* 6. Send our INFO */
    if (!send_info(np, fd)) {
        np_msg(np, "Handshake failed (send info)");
        goto fail;
    }

    /* 7. Receive SYNC */
    if (!recv_sync(np, fd)) goto fail;

    /* 8. Start core */
    core_start(np);

    /* 9. Send PLAY (or SPECTATE if we're joining as spectator) */
    if (np->spectating) {
        if (!send_spectate(np, fd)) {
            np_msg(np, "Handshake failed (send spectate)");
            goto fail;
        }
    } else {
        if (!send_play(np, fd)) {
            np_msg(np, "Handshake failed (send play)");
            goto fail;
        }
    }

    /* 10. Receive MODE */
    if (!recv_mode(np, fd)) goto fail;

    /* Switch to non-blocking for data phase */
    set_nonblocking(fd);

    peer->playing = !np->spectating;
    if (np->spectating)
        np_msgf(np, "Spectating host (Player %u)", np->client_id);
    else
        np_msgf(np, "Connected to host as Player %u", np->client_id);
    return true;

fail:
    core_stop(np);
    close(fd);
    free(peer);
    np->peers[0] = NULL;
    np->peer_count = 0;
    return false;
}

bool libra_np_poll(struct libra_netplay *np)
{
    if (!np) return false;

    /* Host: accept new connection (at most one per poll to avoid stalls) */
    if (np->is_host && np->listen_fd >= 0 && np->peer_count < NP_MAX_PEERS) {
        struct sockaddr_storage addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(np->listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd >= 0) {
            set_nodelay(fd);

            struct np_peer *peer = calloc(1, sizeof(*peer));
            if (!peer) {
                close(fd);
                goto poll_data;
            }
            peer->fd = fd;
            peer->client_id = np->next_client_id++;

            /* --- Server handshake (blocking, with timeout) --- */

            /* 1. Receive client header */
            uint32_t proto;
            if (!recv_header(np, fd, &proto)) {
                np_msg(np, "Client handshake failed (header)");
                close(fd); free(peer);
                goto poll_data;
            }

            /* 2. Send our header with negotiated protocol */
            if (!send_header(np, fd, proto)) {
                close(fd); free(peer);
                goto poll_data;
            }

            /* 3. Receive client NICK */
            if (!recv_nick(np, fd, peer->nick)) {
                close(fd); free(peer);
                goto poll_data;
            }

            /* 4. Send our NICK */
            if (!send_nick(np, fd)) {
                close(fd); free(peer);
                goto poll_data;
            }

            /* 5. Send INFO */
            if (!send_info(np, fd)) {
                close(fd); free(peer);
                goto poll_data;
            }

            /* 6. Receive client INFO */
            if (!recv_info(np, fd)) {
                close(fd); free(peer);
                goto poll_data;
            }

            /* 7. Send SYNC */
            if (!send_sync(np, fd, peer->client_id, peer->nick)) {
                close(fd); free(peer);
                goto poll_data;
            }

            /* 8. Receive PLAY or SPECTATE */
            bool is_spectate = false;
            if (!recv_play_or_spectate(np, fd, &is_spectate)) {
                close(fd); free(peer);
                goto poll_data;
            }

            /* 9. Send MODE (accepted or spectator) */
            if (is_spectate) {
                if (!send_mode_spectator(np, fd, peer->client_id, peer->nick)) {
                    close(fd); free(peer);
                    goto poll_data;
                }
            } else {
                if (!send_mode_accepted(np, fd, peer->client_id, peer->nick)) {
                    close(fd); free(peer);
                    goto poll_data;
                }
            }

            /* 10. Notify core (only for playing peers) */
            if (!is_spectate) {
                if (!core_connected(np, peer->client_id)) {
                    np_msg(np, "Core rejected connection");
                    close(fd); free(peer);
                    goto poll_data;
                }
            }

            /* Switch to non-blocking for data phase */
            set_nonblocking(fd);

            peer->playing    = !is_spectate;
            peer->spectating = is_spectate;
            np->peers[np->peer_count++] = peer;

            if (is_spectate)
                np_msgf(np, "Spectator %u connected: %s",
                        peer->client_id, peer->nick);
            else
                np_msgf(np, "Player %u connected: %s",
                        peer->client_id, peer->nick);
        }
    }

poll_data:
    /* Read and dispatch packets from all connected peers (including spectators
     * so we process DISCONNECT / PING from them) */
    for (int i = 0; i < np->peer_count; i++) {
        if (np->peers[i] && (np->peers[i]->playing || np->peers[i]->spectating))
            read_and_dispatch_peer(np, np->peers[i]);
    }

    /* Remove disconnected peers (iterate in reverse to compact safely) */
    for (int i = np->peer_count - 1; i >= 0; i--) {
        struct np_peer *peer = np->peers[i];
        if (!peer || peer->fd < 0) {
            if (peer) {
                if (np->is_host) {
                    np_msgf(np, "Player %u disconnected", peer->client_id);
                    core_disconnected(np, peer->client_id);
                }
                free(peer);
            }
            /* Compact: shift remaining peers down */
            for (int j = i; j < np->peer_count - 1; j++)
                np->peers[j] = np->peers[j + 1];
            np->peers[--np->peer_count] = NULL;
        }
    }

    /* Client: if host connection lost, stop */
    if (!np->is_host && np->peer_count == 0 && np->core_started) {
        np_msg(np, "Connection lost");
        core_stop(np);
        return false;
    }

    /* Call core's per-frame poll (once, not per-peer) */
    if (np->core_started && np->ctx->has_netpacket &&
        np->ctx->netpacket_cb.poll) {
        s_np = np;
        libra_environment_set_ctx(np->ctx);
        np->ctx->netpacket_cb.poll();
    }

    return true;
}

void libra_np_disconnect(struct libra_netplay *np)
{
    if (!np) return;

    /* Send disconnect to all peers */
    for (int i = 0; i < np->peer_count; i++) {
        if (np->peers[i]) {
            if (np->peers[i]->fd >= 0) {
                write_cmd(np->peers[i]->fd, CMD_DISCONNECT, NULL, 0);
                if (np->is_host)
                    core_disconnected(np, np->peers[i]->client_id);
                close(np->peers[i]->fd);
            }
            free(np->peers[i]);
            np->peers[i] = NULL;
        }
    }
    np->peer_count = 0;

    core_stop(np);

    if (np->listen_fd >= 0) {
        close(np->listen_fd);
        np->listen_fd = -1;
    }

    np->core_started = false;
}

bool libra_np_active(const struct libra_netplay *np)
{
    if (!np) return false;
    return np->listen_fd >= 0 || np->peer_count > 0;
}

bool libra_np_connected(const struct libra_netplay *np)
{
    if (!np) return false;
    for (int i = 0; i < np->peer_count; i++) {
        if (np->peers[i] && np->peers[i]->playing)
            return true;
    }
    return false;
}

bool libra_np_is_host(const struct libra_netplay *np)
{
    if (!np) return false;
    return np->is_host;
}

bool libra_np_is_waiting(const struct libra_netplay *np)
{
    if (!np) return false;
    return np->listen_fd >= 0 && np->peer_count == 0;
}

uint16_t libra_np_client_id(const struct libra_netplay *np)
{
    if (!np) return 0;
    return np->client_id;
}

unsigned libra_np_peer_count(const struct libra_netplay *np)
{
    if (!np) return 0;
    unsigned count = 0;
    for (int i = 0; i < np->peer_count; i++) {
        if (np->peers[i] && np->peers[i]->playing)
            count++;
    }
    return count;
}

bool libra_np_spectate(struct libra_netplay *np)
{
    if (!np) return false;
    np->spectating = true;
    return true;
}

bool libra_np_is_spectating(const struct libra_netplay *np)
{
    if (!np) return false;
    return np->spectating;
}

unsigned libra_np_spectator_count(const struct libra_netplay *np)
{
    if (!np) return 0;
    unsigned count = 0;
    for (int i = 0; i < np->peer_count; i++) {
        if (np->peers[i] && np->peers[i]->spectating)
            count++;
    }
    return count;
}

/* -------------------------------------------------------------------------
 * Relay host/join (netpacket mode over MITM tunnel)
 * ---------------------------------------------------------------------- */

bool libra_np_host_relay(struct libra_netplay *np,
                          const char *relay_ip, uint16_t relay_port,
                          uint8_t mitm_id_out[MITM_ID_SIZE],
                          libra_net_message_cb_t msg_cb)
{
    if (!np || !np->ctx->has_netpacket || !relay_ip) return false;
    libra_np_disconnect(np);

    np->msg_cb = msg_cb;
    np->is_host = true;
    np->client_id = 0;
    np->next_client_id = 1;

    /* Connect to relay and create session */
    int fd = np_relay_create_session(np, relay_ip,
                relay_port ? relay_port : NP_DEFAULT_PORT,
                mitm_id_out);
    if (fd < 0) return false;

    /* The relay fd acts as a listen-like fd: the next bytes
     * will be the client's handshake (relayed through the MITM).
     * We treat it as a direct accept-like connection. */

    struct np_peer *peer = calloc(1, sizeof(*peer));
    if (!peer) { close(fd); return false; }
    peer->fd = fd;
    peer->client_id = np->next_client_id++;

    /* Handshake as host on the relay fd */
    uint32_t proto;
    if (!recv_header(np, fd, &proto))   { close(fd); free(peer); return false; }
    if (!send_header(np, fd, proto))    { close(fd); free(peer); return false; }
    if (!recv_nick(np, fd, peer->nick)) { close(fd); free(peer); return false; }
    if (!send_nick(np, fd))             { close(fd); free(peer); return false; }
    if (!send_info(np, fd))             { close(fd); free(peer); return false; }
    if (!recv_info(np, fd))             { close(fd); free(peer); return false; }
    if (!send_sync(np, fd, peer->client_id, peer->nick)) { close(fd); free(peer); return false; }

    bool is_spectate = false;
    if (!recv_play_or_spectate(np, fd, &is_spectate)) { close(fd); free(peer); return false; }

    if (is_spectate) {
        if (!send_mode_spectator(np, fd, peer->client_id, peer->nick)) { close(fd); free(peer); return false; }
    } else {
        if (!send_mode_accepted(np, fd, peer->client_id, peer->nick)) { close(fd); free(peer); return false; }
    }

    if (!is_spectate) {
        if (!core_connected(np, peer->client_id)) {
            np_msg(np, "Core rejected connection");
            close(fd); free(peer); return false;
        }
    }

    set_nonblocking(fd);
    peer->playing = !is_spectate;
    peer->spectating = is_spectate;
    np->peers[np->peer_count++] = peer;

    /* Start core session as host */
    core_start(np);

    np_msgf(np, "Relay: connected to peer via relay");
    return true;
}

bool libra_np_join_relay(struct libra_netplay *np,
                          const char *relay_ip, uint16_t relay_port,
                          const uint8_t mitm_id[MITM_ID_SIZE],
                          libra_net_message_cb_t msg_cb)
{
    if (!np || !np->ctx->has_netpacket || !relay_ip || !mitm_id) return false;
    libra_np_disconnect(np);

    np->msg_cb = msg_cb;
    np->is_host = false;

    int fd = np_relay_join_session(np, relay_ip,
                relay_port ? relay_port : NP_DEFAULT_PORT,
                mitm_id);
    if (fd < 0) return false;

    struct np_peer *peer = calloc(1, sizeof(*peer));
    if (!peer) { close(fd); return false; }
    peer->fd = fd;
    peer->client_id = 0; /* host is 0 */
    np->peers[0] = peer;
    np->peer_count = 1;

    /* Client handshake over relay tunnel */
    if (!send_header(np, fd, NP_PROTO_LO))  goto fail;
    uint32_t proto;
    if (!recv_header(np, fd, &proto))       goto fail;
    if (!send_nick(np, fd))                 goto fail;
    if (!recv_nick(np, fd, peer->nick))     goto fail;
    if (!recv_info(np, fd))                 goto fail;
    if (!send_info(np, fd))                 goto fail;
    if (!recv_sync(np, fd))                 goto fail;

    core_start(np);

    if (np->spectating) {
        if (!send_spectate(np, fd))         goto fail;
    } else {
        if (!send_play(np, fd))             goto fail;
    }
    if (!recv_mode(np, fd))                 goto fail;

    set_nonblocking(fd);
    peer->playing = !np->spectating;

    if (np->spectating)
        np_msg(np, "Relay: spectating via relay");
    else
        np_msgf(np, "Relay: connected as Player %u via relay", np->client_id);
    return true;

fail:
    core_stop(np);
    close(fd);
    free(peer);
    np->peers[0] = NULL;
    np->peer_count = 0;
    return false;
}
