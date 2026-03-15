// SPDX-License-Identifier: MIT
/*
 * libra rollback — savestate-based rollback netplay
 *
 * Clean-room MIT implementation of RetroArch's INPUT_FRAME_SYNC mode.
 * Wire-compatible with RetroArch protocol 5-7.
 * Only interop constants (command IDs, wire format) are reused.
 */
#include "rollback.h"
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
#include <pthread.h>
#include "miniz.h"

/* -------------------------------------------------------------------------
 * Capability flags (advertised in handshake header word 2)
 * These are libra-private extensions; RetroArch always sends 0 so RA peers
 * will have peer_caps=0 and all extension paths stay disabled.
 * ---------------------------------------------------------------------- */
#define RB_CAP_DELTA_SAVESTATES 0x00000001u  /* supports CMD_LOAD_SAVESTATE_DELTA */

/* -------------------------------------------------------------------------
 * Ring buffer
 * ---------------------------------------------------------------------- */

#define RB_BUFFER_SIZE 128   /* must be power of 2 */
#define RB_BUFFER_MASK (RB_BUFFER_SIZE - 1)

#define RB_MAX_STALL   60   /* max frames to stall waiting for peer */
#define RB_CRC_INTERVAL 120 /* check CRC every N frames */

struct rb_frame {
    uint32_t frame_num;
    uint32_t local_input[16];   /* one word per port */
    uint32_t remote_input[16];  /* one word per port */
    bool     have_local;
    bool     have_remote;
    uint32_t crc;               /* 0 = not computed */
    void    *state;             /* serialized core state */
    size_t   state_size;
};

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

struct libra_rollback {
    libra_ctx_t *ctx;
    int          peer_fd;       /* single peer (1v1) */
    bool         is_host;
    bool         connected;
    bool         is_replay;
    uint16_t     client_id;
    uint32_t     self_frame;    /* our current frame count */
    uint32_t     peer_frame;    /* last frame we received from peer */
    unsigned     input_latency; /* additional frames of local delay */
    uint16_t     port;

    /* Ring buffer */
    struct rb_frame buffer[RB_BUFFER_SIZE];
    size_t          state_alloc_size;  /* from libra_serialize_size() */

    /* Receive buffer for TCP stream reassembly */
    uint8_t recv_buf[128 * 1024];
    size_t  recv_len;

    /* Stall tracking */
    int      stall_frames;

    /* CRC check interval */
    unsigned crc_check_interval;

    /* Savestate compression buffer */
    void    *compress_buf;
    size_t   compress_buf_size;

    /* XOR-delta reference for savestate exchange.
     * Set to a copy of every full state sent/received so that subsequent
     * desyncs can send only the diff (much more compressible). */
    void   *delta_ref;
    size_t  delta_ref_size;

    /* Capability flags negotiated during handshake.
     * peer_caps: what the peer advertised; delta_enabled: both sides support deltas. */
    uint32_t peer_caps;
    bool     delta_enabled;

    /* Protocol version (negotiated) */
    uint32_t protocol;

    /* Callback */
    libra_net_message_cb_t msg_cb;
    void *msg_ud;
    char our_nick[32];

    /* Listen socket (host only) */
    int listen_fd;

    /* Pending savestate send request */
    bool savestate_pending;

    /* Number of input words per frame we send/receive.
     * For simple joypad mode this is 1 per port. */
    unsigned local_words;
    unsigned remote_words;

    /* Port assignments: host = port 0, client = port 1 */
    unsigned local_port;
    unsigned remote_port;
};

/* -------------------------------------------------------------------------
 * Messaging helpers
 * ---------------------------------------------------------------------- */

static void rb_msg(struct libra_rollback *rb, const char *msg)
{
    fprintf(stderr, "libra [ROLLBACK] %s\n", msg);
    if (rb->msg_cb)
        rb->msg_cb(rb->msg_ud, msg);
}

static void rb_msgf(struct libra_rollback *rb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void rb_msgf(struct libra_rollback *rb, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rb_msg(rb, buf);
}

/* -------------------------------------------------------------------------
 * Device word count (for wire format)
 * ---------------------------------------------------------------------- */

__attribute__((unused))
static unsigned device_word_count(unsigned device)
{
    switch (device & 0xFF) { /* RETRO_DEVICE_MASK */
        case 1: /* RETRO_DEVICE_JOYPAD  */ return 1;
        case 5: /* RETRO_DEVICE_ANALOG  */ return 3;
        case 2: /* RETRO_DEVICE_MOUSE   */ return 2;
        case 6: /* RETRO_DEVICE_LIGHTGUN*/ return 2;
        case 3: /* RETRO_DEVICE_KEYBOARD*/ return 5;
        default:                           return 0;
    }
}

/* -------------------------------------------------------------------------
 * Relay fd helpers
 * ---------------------------------------------------------------------- */

/* Connect to a relay server and return a TCP fd, or -1 on failure. */
static int rb_relay_connect(struct libra_rollback *rb,
                             const char *ip, uint16_t port)
{
    int fd = netsock_tcp_connect(ip, port);
    if (fd < 0) rb_msg(rb, "Relay connection failed");
    return fd;
}

static int rb_relay_create_session(struct libra_rollback *rb,
                                    const char *ip, uint16_t port,
                                    uint8_t mitm_id_out[MITM_ID_SIZE])
{
    int fd = rb_relay_connect(rb, ip, port);
    if (fd < 0) return -1;

    uint32_t magic = htonl(MITM_LINK_MAGIC);
    if (!netsock_write_all(fd, &magic, 4)) {
        rb_msg(rb, "Relay handshake failed"); close(fd); return -1;
    }
    if (!netsock_read_all(fd, mitm_id_out, MITM_ID_SIZE, 5000)) {
        rb_msg(rb, "Relay handshake failed"); close(fd); return -1;
    }
    return fd;
}

static int rb_relay_join_session(struct libra_rollback *rb,
                                  const char *ip, uint16_t port,
                                  const uint8_t mitm_id[MITM_ID_SIZE])
{
    int fd = rb_relay_connect(rb, ip, port);
    if (fd < 0) return -1;

    uint32_t magic = htonl(MITM_ADDR_MAGIC);
    if (!netsock_write_all(fd, &magic, 4)) {
        rb_msg(rb, "Relay handshake failed"); close(fd); return -1;
    }
    if (!netsock_write_all(fd, mitm_id, MITM_ID_SIZE)) {
        rb_msg(rb, "Relay handshake failed"); close(fd); return -1;
    }
    uint8_t echo[MITM_ID_SIZE];
    if (!netsock_read_all(fd, echo, MITM_ID_SIZE, 5000)) {
        rb_msg(rb, "Relay handshake failed"); close(fd); return -1;
    }
    return fd;
}

/* -------------------------------------------------------------------------
 * Ring buffer helpers
 * ---------------------------------------------------------------------- */

static struct rb_frame *rb_slot(struct libra_rollback *rb, uint32_t frame)
{
    return &rb->buffer[frame & RB_BUFFER_MASK];
}

static void rb_ensure_state_buf(struct libra_rollback *rb, struct rb_frame *f)
{
    if (!f->state && rb->state_alloc_size > 0) {
        f->state = malloc(rb->state_alloc_size);
        f->state_size = rb->state_alloc_size;
    }
}

/* -------------------------------------------------------------------------
 * Handshake (uses netsock helpers)
 * ---------------------------------------------------------------------- */

static bool rb_send_header(struct libra_rollback *rb, int fd, uint32_t proto)
{
    uint32_t hdr[6];
    hdr[0] = htonl(NP_MAGIC);
    hdr[1] = htonl(netsock_platform_magic());
    hdr[2] = htonl(RB_CAP_DELTA_SAVESTATES); /* libra capability flags */

    if (rb->is_host)
        hdr[3] = htonl(0);
    else
        hdr[3] = htonl(NP_PROTO_HI);

    hdr[4] = htonl(proto);
    hdr[5] = htonl(netsock_impl_magic());
    return netsock_write_all(fd, hdr, sizeof(hdr));
}

static bool rb_recv_header(struct libra_rollback *rb, int fd, uint32_t *out_proto)
{
    uint32_t hdr[6];
    if (!netsock_read_all(fd, hdr, sizeof(hdr), 5000))
        return false;

    if (ntohl(hdr[0]) != NP_MAGIC) {
        rb_msg(rb, "Not a compatible netplay peer");
        return false;
    }

    /* Store peer capability flags (RetroArch always sends 0 here) */
    rb->peer_caps = ntohl(hdr[2]);

    uint32_t lo_proto = ntohl(hdr[4]);
    uint32_t hi_proto = ntohl(hdr[3]);

    if (rb->is_host) {
        uint32_t best = hi_proto ? hi_proto : lo_proto;
        if (best > NP_PROTO_HI) best = NP_PROTO_HI;
        if (best < NP_PROTO_LO) {
            rb_msg(rb, "Peer protocol version too old");
            return false;
        }
        *out_proto = best;
    } else {
        *out_proto = lo_proto;
        if (*out_proto < NP_PROTO_LO || *out_proto > NP_PROTO_HI) {
            rb_msg(rb, "Server protocol version incompatible");
            return false;
        }
    }
    return true;
}

static bool rb_send_nick(struct libra_rollback *rb, int fd)
{
    uint8_t payload[NP_NICK_LEN];
    memset(payload, 0, sizeof(payload));
    strncpy((char *)payload, rb->our_nick, NP_NICK_LEN - 1);
    return netsock_write_cmd(fd, CMD_NICK, payload, NP_NICK_LEN);
}

static bool rb_recv_nick(struct libra_rollback *rb, int fd, char *nick_out)
{
    uint32_t hdr[2];
    if (!netsock_read_all(fd, hdr, 8, 5000)) return false;
    if (ntohl(hdr[0]) != CMD_NICK || ntohl(hdr[1]) != NP_NICK_LEN) {
        rb_msg(rb, "Expected NICK command");
        return false;
    }
    if (!netsock_read_all(fd, nick_out, NP_NICK_LEN, 5000)) return false;
    nick_out[NP_NICK_LEN - 1] = '\0';
    return true;
}

static bool rb_send_info(struct libra_rollback *rb, int fd)
{
    uint8_t payload[68];
    memset(payload, 0, sizeof(payload));
    uint32_t crc = htonl(0);
    memcpy(payload, &crc, 4);

    const char *name = rb->ctx->core ? rb->ctx->core->sys_info.library_name : "unknown";
    if (name) strncpy((char *)payload + 4, name, NP_NICK_LEN - 1);

    const char *ver = rb->ctx->core ? rb->ctx->core->sys_info.library_version : NULL;
    if (ver) strncpy((char *)payload + 36, ver, NP_NICK_LEN - 1);

    return netsock_write_cmd(fd, CMD_INFO, payload, sizeof(payload));
}

static bool rb_recv_info(struct libra_rollback *rb, int fd)
{
    uint32_t hdr[2];
    if (!netsock_read_all(fd, hdr, 8, 5000)) return false;
    if (ntohl(hdr[0]) != CMD_INFO) {
        rb_msg(rb, "Expected INFO command");
        return false;
    }

    uint32_t sz = ntohl(hdr[1]);
    if (sz < 68) {
        rb_msg(rb, "INFO payload too small");
        return false;
    }

    uint8_t payload[68];
    if (!netsock_read_all(fd, payload, 68, 5000)) return false;

    if (sz > 68) {
        uint8_t skip[256];
        uint32_t remaining = sz - 68;
        while (remaining > 0) {
            uint32_t chunk = remaining > sizeof(skip) ? sizeof(skip) : remaining;
            if (!netsock_read_all(fd, skip, chunk, 5000)) return false;
            remaining -= chunk;
        }
    }

    /* Validate core name */
    char peer_core[NP_NICK_LEN];
    memcpy(peer_core, payload + 4, NP_NICK_LEN);
    peer_core[NP_NICK_LEN - 1] = '\0';

    const char *our_core = rb->ctx->core
        ? rb->ctx->core->sys_info.library_name : "unknown";
    if (our_core && peer_core[0] && strcasecmp(peer_core, our_core) != 0) {
        rb_msgf(rb, "Incompatible core: %s vs %s", peer_core, our_core);
        return false;
    }
    return true;
}

/* Host sends SYNC with SRAM appended */
static bool rb_send_sync(struct libra_rollback *rb, int fd,
                          uint32_t client_num, const char *nick)
{
    /* SYNC payload: 184 fixed bytes + SRAM data */
    size_t sram_size = 0;
    void *sram_data = NULL;
    if (rb->ctx->core && rb->ctx->core->retro_get_memory_size &&
        rb->ctx->core->retro_get_memory_data) {
        sram_size = rb->ctx->core->retro_get_memory_size(0); /* SAVE_RAM */
        sram_data = rb->ctx->core->retro_get_memory_data(0);
    }

    uint32_t payload_size = 184 + (uint32_t)sram_size;
    uint8_t *payload = (uint8_t *)calloc(1, payload_size);
    if (!payload) return false;

    size_t off = 0;
    uint32_t val;

    /* frame_count = current frame */
    val = htonl(rb->self_frame);
    memcpy(payload + off, &val, 4); off += 4;

    /* client_num */
    val = htonl(client_num);
    memcpy(payload + off, &val, 4); off += 4;

    /* config_devices[16] */
    for (unsigned p = 0; p < 16; p++) {
        unsigned dev = (p < rb->ctx->port_count) ? rb->ctx->port_devices[p] : 1;
        val = htonl(dev);
        memcpy(payload + off, &val, 4);
        off += 4;
    }

    /* share_modes[16] (1 byte each) */
    off += 16;

    /* device_clients[16] — port 0 → host (0), port 1 → client (client_num) */
    val = htonl(0); /* port 0 = host */
    memcpy(payload + off, &val, 4);
    off += 4;
    val = htonl(client_num); /* port 1 = client */
    memcpy(payload + off, &val, 4);
    off += 4;
    /* rest are 0 */
    off += (16 - 2) * 4;

    /* nick */
    strncpy((char *)payload + off, nick, NP_NICK_LEN - 1);
    off += NP_NICK_LEN;

    /* Append SRAM data */
    if (sram_size > 0 && sram_data)
        memcpy(payload + off, sram_data, sram_size);

    bool ok = netsock_write_cmd(fd, CMD_SYNC, payload, payload_size);
    free(payload);
    return ok;
}

/* Client receives SYNC — applies SRAM and device config */
static bool rb_recv_sync(struct libra_rollback *rb, int fd)
{
    uint32_t hdr[2];
    if (!netsock_read_all(fd, hdr, 8, 5000)) return false;
    if (ntohl(hdr[0]) != CMD_SYNC) {
        rb_msg(rb, "Expected SYNC command");
        return false;
    }

    uint32_t sz = ntohl(hdr[1]);
    if (sz < 184) {
        rb_msg(rb, "SYNC payload too small");
        return false;
    }

    /* Read frame_count + client_num */
    uint32_t fc, cn;
    if (!netsock_read_all(fd, &fc, 4, 5000)) return false;
    if (!netsock_read_all(fd, &cn, 4, 5000)) return false;

    rb->self_frame = ntohl(fc);
    rb->peer_frame = rb->self_frame;
    rb->client_id = (uint16_t)(ntohl(cn) & 0x7FFFFFFF);

    /* Read config_devices[16] */
    uint32_t devices[16];
    if (!netsock_read_all(fd, devices, 64, 5000)) return false;

    /* Apply device types */
    for (unsigned p = 0; p < 16; p++) {
        unsigned dev = ntohl(devices[p]);
        if (dev != 0 && p < 16) {
            rb->ctx->port_devices[p] = dev;
            if (p >= rb->ctx->port_count)
                rb->ctx->port_count = p + 1;
        }
    }

    /* Skip share_modes(16) + device_clients(64) + nick(32) = 112 bytes */
    uint8_t skip[112];
    if (!netsock_read_all(fd, skip, 112, 5000)) return false;

    /* Read remaining SRAM data */
    uint32_t sram_len = sz - 184;
    if (sram_len > 0) {
        void *sram_buf = malloc(sram_len);
        if (!sram_buf) return false;
        if (!netsock_read_all(fd, sram_buf, sram_len, 5000)) {
            free(sram_buf);
            return false;
        }

        /* Apply SRAM */
        if (rb->ctx->core && rb->ctx->core->retro_get_memory_data &&
            rb->ctx->core->retro_get_memory_size) {
            size_t core_sram = rb->ctx->core->retro_get_memory_size(0);
            void *dest = rb->ctx->core->retro_get_memory_data(0);
            if (dest && core_sram > 0) {
                size_t copy = sram_len < core_sram ? sram_len : core_sram;
                memcpy(dest, sram_buf, copy);
            }
        }
        free(sram_buf);
    }

    return true;
}

static bool rb_send_play(struct libra_rollback *rb, int fd)
{
    (void)rb;
    uint32_t mode = htonl(0);
    return netsock_write_cmd(fd, CMD_PLAY, &mode, sizeof(mode));
}

static bool rb_send_mode_accepted(struct libra_rollback *rb, int fd,
                                   uint32_t client_num, const char *nick)
{
    (void)rb;
    uint8_t payload[60];
    memset(payload, 0, sizeof(payload));

    uint32_t val = htonl(0);
    memcpy(payload, &val, 4);

    val = htonl(MODE_BIT_YOU | MODE_BIT_PLAYING | client_num);
    memcpy(payload + 4, &val, 4);

    strncpy((char *)payload + 28, nick, NP_NICK_LEN - 1);
    return netsock_write_cmd(fd, CMD_MODE, payload, sizeof(payload));
}

static bool rb_recv_mode(struct libra_rollback *rb, int fd)
{
    uint32_t hdr[2];
    if (!netsock_read_all(fd, hdr, 8, 5000)) return false;

    uint32_t cmd = ntohl(hdr[0]);
    uint32_t sz  = ntohl(hdr[1]);

    if (cmd == CMD_MODE_REFUSED) {
        uint8_t skip[64];
        while (sz > 0) {
            uint32_t chunk = sz > sizeof(skip) ? sizeof(skip) : sz;
            if (!netsock_read_all(fd, skip, chunk, 5000)) return false;
            sz -= chunk;
        }
        rb_msg(rb, "Server refused play request");
        return false;
    }

    if (cmd != CMD_MODE || sz < 60) {
        rb_msg(rb, "Expected MODE command");
        return false;
    }

    uint8_t payload[60];
    if (!netsock_read_all(fd, payload, 60, 5000)) return false;

    if (sz > 60) {
        uint8_t skip[64];
        uint32_t remaining = sz - 60;
        while (remaining > 0) {
            uint32_t chunk = remaining > sizeof(skip) ? sizeof(skip) : remaining;
            if (!netsock_read_all(fd, skip, chunk, 5000)) return false;
            remaining -= chunk;
        }
    }

    uint32_t mode_bits;
    memcpy(&mode_bits, payload + 4, 4);
    mode_bits = ntohl(mode_bits);

    if (!(mode_bits & MODE_BIT_PLAYING)) {
        rb_msg(rb, "Server did not grant playing mode");
        return false;
    }
    return true;
}

static bool rb_recv_play(struct libra_rollback *rb, int fd)
{
    uint32_t hdr[2];
    if (!netsock_read_all(fd, hdr, 8, 5000)) return false;

    uint32_t cmd = ntohl(hdr[0]);
    uint32_t sz  = ntohl(hdr[1]);

    if (cmd != CMD_PLAY) {
        uint8_t skip[256];
        while (sz > 0) {
            uint32_t chunk = sz > sizeof(skip) ? sizeof(skip) : sz;
            if (!netsock_read_all(fd, skip, chunk, 5000)) return false;
            sz -= chunk;
        }
        if (!netsock_read_all(fd, hdr, 8, 5000)) return false;
        cmd = ntohl(hdr[0]);
        sz  = ntohl(hdr[1]);
        if (cmd != CMD_PLAY) {
            rb_msg(rb, "Expected PLAY command from client");
            return false;
        }
    }

    if (sz > 0) {
        uint8_t skip[64];
        while (sz > 0) {
            uint32_t chunk = sz > sizeof(skip) ? sizeof(skip) : sz;
            if (!netsock_read_all(fd, skip, chunk, 5000)) return false;
            sz -= chunk;
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Send INPUT command
 * ---------------------------------------------------------------------- */

static bool rb_send_input(struct libra_rollback *rb, uint32_t frame,
                           const uint32_t *input, unsigned nwords)
{
    /* INPUT wire format:
     * payload = frame_num(4) + client_id(4) + is_server_data(1) +
     *           pad(3) + input_words(nwords*4)
     * Total = 12 + nwords*4 */
    uint32_t payload_size = 12 + nwords * 4;
    uint8_t buf[12 + 16 * 4]; /* max 16 words */
    if (payload_size > sizeof(buf)) return false;

    uint32_t val;
    val = htonl(frame);
    memcpy(buf, &val, 4);

    val = htonl((uint32_t)rb->client_id);
    memcpy(buf + 4, &val, 4);

    /* is_server_data(1) + pad(3) */
    memset(buf + 8, 0, 4);

    for (unsigned i = 0; i < nwords; i++) {
        val = htonl(input[i]);
        memcpy(buf + 12 + i * 4, &val, 4);
    }

    return netsock_write_cmd(rb->peer_fd, CMD_INPUT, buf, payload_size);
}

/* -------------------------------------------------------------------------
 * Send NOINPUT (stall notification)
 * ---------------------------------------------------------------------- */

static bool rb_send_noinput(struct libra_rollback *rb, uint32_t frame)
{
    uint32_t payload[2];
    payload[0] = htonl(frame);
    payload[1] = htonl((uint32_t)rb->client_id);
    return netsock_write_cmd(rb->peer_fd, CMD_NOINPUT, payload, 8);
}

/* -------------------------------------------------------------------------
 * Send CRC command
 * ---------------------------------------------------------------------- */

static bool rb_send_crc(struct libra_rollback *rb, uint32_t frame, uint32_t crc)
{
    uint32_t payload[2];
    payload[0] = htonl(frame);
    payload[1] = htonl(crc);
    return netsock_write_cmd(rb->peer_fd, CMD_CRC, payload, 8);
}

/* -------------------------------------------------------------------------
 * NETPLAY1 container wrap/unwrap (protocol 7)
 * ---------------------------------------------------------------------- */

/* Wrap raw state into a NETPLAY1 container. Returns malloc'd buffer, sets *out_len. */
__attribute__((unused))
static void *netplay1_wrap(const void *state, size_t state_size, size_t *out_len)
{
    /* "NETPLAY" (7) + version(1) + "MEM " (4) + size(4) + data(padded) + "END " (4) + 0(4) */
    size_t padded = (state_size + 7) & ~(size_t)7; /* 8-byte alignment */
    size_t total = 7 + 1 + 4 + 4 + padded + 4 + 4;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return NULL;

    size_t off = 0;
    memcpy(buf + off, "NETPLAY", 7); off += 7;
    buf[off++] = 0x01; /* version */
    memcpy(buf + off, "MEM ", 4); off += 4;

    /* state_size as little-endian u32 */
    uint32_t sz_le = (uint32_t)state_size;
    buf[off+0] = (uint8_t)(sz_le);
    buf[off+1] = (uint8_t)(sz_le >> 8);
    buf[off+2] = (uint8_t)(sz_le >> 16);
    buf[off+3] = (uint8_t)(sz_le >> 24);
    off += 4;

    memcpy(buf + off, state, state_size);
    off += padded;

    memcpy(buf + off, "END ", 4); off += 4;
    memset(buf + off, 0, 4); off += 4;

    *out_len = total;
    return buf;
}

/* Unwrap a NETPLAY1 container. Returns pointer to state data within buf, sets *state_size.
 * Returns NULL if not a valid container. */
static const void *netplay1_unwrap(const void *buf, size_t buf_len, size_t *state_size)
{
    const uint8_t *p = (const uint8_t *)buf;
    if (buf_len < 20) return NULL;
    if (memcmp(p, "NETPLAY", 7) != 0) return NULL;
    if (p[7] != 0x01) return NULL;
    if (memcmp(p + 8, "MEM ", 4) != 0) return NULL;

    uint32_t sz = (uint32_t)p[12] | ((uint32_t)p[13] << 8)
                | ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    if (16 + sz > buf_len) return NULL;

    *state_size = sz;
    return p + 16;
}

/* -------------------------------------------------------------------------
 * Compressed savestate send/receive
 * ---------------------------------------------------------------------- */

/* Update delta reference — copy state into delta_ref, (re)allocating as needed. */
static void rb_update_delta_ref(struct libra_rollback *rb, const void *state, size_t sz)
{
    if (rb->delta_ref_size != sz) {
        free(rb->delta_ref);
        rb->delta_ref = malloc(sz);
        rb->delta_ref_size = rb->delta_ref ? sz : 0;
    }
    if (rb->delta_ref)
        memcpy(rb->delta_ref, state, sz);
}

static bool rb_send_savestate(struct libra_rollback *rb, uint32_t frame)
{
    libra_ctx_t *ctx = rb->ctx;
    size_t sz = libra_serialize_size(ctx);
    if (sz == 0) return false;

    void *state = malloc(sz);
    if (!state) return false;

    libra_environment_set_ctx(ctx);
    if (!libra_serialize(ctx, state, sz)) {
        free(state);
        return false;
    }

    /* Decide whether to send a full state or an XOR delta.
     * Only use delta when both peers negotiated the capability; otherwise
     * always send full states (ensures interop with RetroArch peers). */
    bool use_delta = rb->delta_enabled &&
                     rb->delta_ref != NULL &&
                     rb->delta_ref_size == sz;

    /* If delta: compute XOR in-place into a temp buffer */
    void *payload_data = state;  /* points to data to compress */
    void *delta_buf    = NULL;
    if (use_delta) {
        delta_buf = malloc(sz);
        if (!delta_buf) {
            use_delta = false;
        } else {
            const uint8_t *ref = (const uint8_t *)rb->delta_ref;
            const uint8_t *src = (const uint8_t *)state;
            uint8_t *dst       = (uint8_t *)delta_buf;
            for (size_t i = 0; i < sz; i++)
                dst[i] = src[i] ^ ref[i];
            payload_data = delta_buf;
        }
    }

    /* CRC of the FULL (non-delta) state for integrity verification */
    uint32_t crc = (uint32_t)crc32(0L, (const Bytef *)state, (uInt)sz);

    /* Compress */
    uLongf comp_sz = compressBound((uLong)sz);
    if (comp_sz + 256 > rb->compress_buf_size) {
        free(rb->compress_buf);
        rb->compress_buf_size = comp_sz + 256;
        rb->compress_buf = malloc(rb->compress_buf_size);
        if (!rb->compress_buf) {
            rb->compress_buf_size = 0;
            free(state); free(delta_buf);
            return false;
        }
    }

    uLongf actual_comp = comp_sz;
    if (compress2((Bytef *)rb->compress_buf, &actual_comp,
                  (const Bytef *)payload_data, (uLong)sz, Z_BEST_SPEED) != Z_OK) {
        free(state); free(delta_buf);
        return false;
    }
    free(delta_buf);

    /* Update delta reference BEFORE freeing state */
    rb_update_delta_ref(rb, state, sz);
    free(state);

    /* Build payload: frame(4) + orig_size(4) + crc(4) + compressed_data */
    uint32_t payload_size = 12 + (uint32_t)actual_comp;
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (!payload) return false;

    uint32_t val;
    val = htonl(frame);           memcpy(payload,     &val, 4);
    val = htonl((uint32_t)sz);   memcpy(payload + 4, &val, 4);
    val = htonl(crc);            memcpy(payload + 8, &val, 4);
    memcpy(payload + 12, rb->compress_buf, actual_comp);

    uint32_t cmd = use_delta ? CMD_LOAD_SAVESTATE_DELTA : CMD_LOAD_SAVESTATE;
    bool ok = netsock_write_cmd(rb->peer_fd, cmd, payload, payload_size);
    free(payload);
    return ok;
}

/* -------------------------------------------------------------------------
 * Background compression — pipelined with handshake I/O
 * ---------------------------------------------------------------------- */

struct rb_compress_job {
    const void *input;       /* raw state (caller owns, must outlive thread) */
    size_t      input_size;
    void       *output;      /* malloc'd by worker, caller must free */
    size_t      output_size;
    uint32_t    crc;         /* CRC32 of input, computed by worker */
    int         result;      /* Z_OK on success */
};

static void *rb_compress_worker(void *arg)
{
    struct rb_compress_job *job = (struct rb_compress_job *)arg;
    job->crc = (uint32_t)crc32(0L, (const Bytef *)job->input, (uInt)job->input_size);
    uLongf bound = compressBound((uLong)job->input_size);
    job->output = malloc((size_t)bound);
    if (!job->output) { job->result = Z_MEM_ERROR; return NULL; }
    uLongf actual = bound;
    job->result = compress2((Bytef *)job->output, &actual,
                            (const Bytef *)job->input, (uLong)job->input_size,
                            Z_BEST_SPEED);
    job->output_size = (size_t)actual;
    return NULL;
}

/* Send a pre-compressed full savestate (no delta — used for initial sync only).
 * Updates delta_ref so subsequent desync states can use XOR delta. */
static bool rb_send_precompressed(struct libra_rollback *rb, uint32_t frame,
                                   const void *raw_state, size_t raw_size,
                                   const void *comp_data, size_t comp_size,
                                   uint32_t crc)
{
    uint32_t payload_size = 12 + (uint32_t)comp_size;
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (!payload) return false;

    uint32_t val;
    val = htonl(frame);              memcpy(payload,      &val, 4);
    val = htonl((uint32_t)raw_size); memcpy(payload + 4,  &val, 4);
    val = htonl(crc);                memcpy(payload + 8,  &val, 4);
    memcpy(payload + 12, comp_data, comp_size);

    /* Establish delta reference so desync recovery can use XOR deltas */
    rb_update_delta_ref(rb, raw_state, raw_size);

    bool ok = netsock_write_cmd(rb->peer_fd, CMD_LOAD_SAVESTATE, payload, payload_size);
    free(payload);
    return ok;
}

/* -------------------------------------------------------------------------
 * Network receive and command dispatch
 * ---------------------------------------------------------------------- */

/* Read available data from peer (non-blocking) */
static bool rb_recv_data(struct libra_rollback *rb)
{
    if (rb->peer_fd < 0) return false;

    size_t space = sizeof(rb->recv_buf) - rb->recv_len;
    if (space == 0) return true; /* buffer full — process first */

    ssize_t n = recv(rb->peer_fd, rb->recv_buf + rb->recv_len, space, 0);
    if (n > 0) {
        rb->recv_len += (size_t)n;
    } else if (n == 0) {
        /* Connection closed */
        close(rb->peer_fd);
        rb->peer_fd = -1;
        rb->connected = false;
        return false;
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(rb->peer_fd);
            rb->peer_fd = -1;
            rb->connected = false;
            return false;
        }
    }
    return true;
}

/* Process one command from recv_buf at offset *pos.
 * Returns false if we need more data. */
static bool rb_dispatch_cmd(struct libra_rollback *rb, size_t *pos)
{
    size_t avail = rb->recv_len - *pos;
    if (avail < 8) return false;

    uint32_t raw_cmd, raw_sz;
    memcpy(&raw_cmd, rb->recv_buf + *pos, 4);
    memcpy(&raw_sz,  rb->recv_buf + *pos + 4, 4);
    uint32_t cmd = ntohl(raw_cmd);
    uint32_t sz  = ntohl(raw_sz);

    if (avail < 8 + sz) return false; /* incomplete */
    *pos += 8;

    switch (cmd) {
        case CMD_INPUT: {
            /* payload: frame(4) + client_id(4) + is_server_data(1) + pad(3) + words... */
            if (sz < 12) { *pos += sz; break; }
            uint32_t frame_n, cid_n;
            memcpy(&frame_n, rb->recv_buf + *pos, 4);
            memcpy(&cid_n,   rb->recv_buf + *pos + 4, 4);
            uint32_t frame = ntohl(frame_n);
            (void)cid_n; /* we know it's the peer */

            unsigned nwords = (sz - 12) / 4;
            struct rb_frame *f = rb_slot(rb, frame);
            f->frame_num = frame;
            for (unsigned i = 0; i < nwords && i < 16; i++) {
                uint32_t w;
                memcpy(&w, rb->recv_buf + *pos + 12 + i * 4, 4);
                f->remote_input[i] = ntohl(w);
            }
            f->have_remote = true;

            if (frame > rb->peer_frame)
                rb->peer_frame = frame;

            *pos += sz;
            break;
        }

        case CMD_NOINPUT: {
            /* peer is stalling — just update peer frame */
            if (sz >= 4) {
                uint32_t frame_n;
                memcpy(&frame_n, rb->recv_buf + *pos, 4);
                /* Don't advance peer_frame for NOINPUT */
            }
            *pos += sz;
            break;
        }

        case CMD_CRC: {
            if (sz < 8) { *pos += sz; break; }
            uint32_t frame_n, crc_n;
            memcpy(&frame_n, rb->recv_buf + *pos, 4);
            memcpy(&crc_n,   rb->recv_buf + *pos + 4, 4);
            uint32_t frame = ntohl(frame_n);
            uint32_t peer_crc = ntohl(crc_n);

            struct rb_frame *f = rb_slot(rb, frame);
            if (f->crc != 0 && peer_crc != 0 && f->crc != peer_crc) {
                rb_msgf(rb, "Desync detected at frame %u (local=%08X peer=%08X)",
                        frame, f->crc, peer_crc);
                /* Request savestate from host to resync */
                if (!rb->is_host) {
                    netsock_write_cmd(rb->peer_fd, CMD_REQUEST_SAVESTATE, NULL, 0);
                } else {
                    rb->savestate_pending = true;
                }
            }
            *pos += sz;
            break;
        }

        case CMD_REQUEST_SAVESTATE: {
            /* Peer requests a savestate (only host should respond) */
            if (rb->is_host)
                rb->savestate_pending = true;
            *pos += sz;
            break;
        }

        case CMD_LOAD_SAVESTATE:
        case CMD_LOAD_SAVESTATE_DELTA: {
            /* Full state payload:  frame(4) + orig_size(4) + crc(4) + compressed_data
             * Delta state payload: same layout but compressed XOR-diff vs delta_ref */
            if (sz < 12) { *pos += sz; break; }
            uint32_t frame_n, orig_n, crc_n;
            memcpy(&frame_n, rb->recv_buf + *pos, 4);
            memcpy(&orig_n,  rb->recv_buf + *pos + 4, 4);
            memcpy(&crc_n,   rb->recv_buf + *pos + 8, 4);
            uint32_t frame     = ntohl(frame_n);
            uint32_t orig_size = ntohl(orig_n);
            (void)crc_n;
            bool is_delta = (cmd == CMD_LOAD_SAVESTATE_DELTA);

            uint32_t comp_size = sz - 12;
            const uint8_t *comp_data = rb->recv_buf + *pos + 12;

            /* Decompress */
            void *state = malloc(orig_size);
            if (state) {
                uLongf dest_len = orig_size;
                if (uncompress((Bytef *)state, &dest_len,
                               comp_data, comp_size) == Z_OK) {
                    /* If delta: XOR with our reference to reconstruct full state */
                    if (is_delta && rb->delta_ref && rb->delta_ref_size == dest_len) {
                        uint8_t *s = (uint8_t *)state;
                        const uint8_t *ref = (const uint8_t *)rb->delta_ref;
                        for (uLongf i = 0; i < dest_len; i++)
                            s[i] ^= ref[i];
                    }

                    /* Update delta reference with the now-full state */
                    rb_update_delta_ref(rb, state, (size_t)dest_len);

                    /* Check for NETPLAY1 container */
                    size_t inner_size;
                    const void *inner = netplay1_unwrap(state, dest_len, &inner_size);
                    if (inner) {
                        libra_environment_set_ctx(rb->ctx);
                        libra_unserialize(rb->ctx, inner, inner_size);
                    } else {
                        libra_environment_set_ctx(rb->ctx);
                        libra_unserialize(rb->ctx, state, dest_len);
                    }
                    /* Reset frame counters to resync */
                    rb->self_frame = frame;
                    rb->peer_frame = frame;

                    /* Clear ring buffer predictions */
                    for (int i = 0; i < RB_BUFFER_SIZE; i++) {
                        rb->buffer[i].have_local = false;
                        rb->buffer[i].have_remote = false;
                        rb->buffer[i].crc = 0;
                    }

                    rb_msgf(rb, "Loaded %s at frame %u",
                            is_delta ? "delta savestate" : "savestate", frame);
                }
                free(state);
            }
            *pos += sz;
            break;
        }

        case CMD_STALL: {
            if (sz >= 4) {
                uint32_t stall_n;
                memcpy(&stall_n, rb->recv_buf + *pos, 4);
                rb->stall_frames = (int)ntohl(stall_n);
            }
            *pos += sz;
            break;
        }

        case CMD_DISCONNECT:
            *pos += sz;
            if (rb->peer_fd >= 0) {
                close(rb->peer_fd);
                rb->peer_fd = -1;
            }
            rb->connected = false;
            return true;

        case CMD_PING_REQUEST:
            *pos += sz;
            if (rb->peer_fd >= 0)
                netsock_write_cmd(rb->peer_fd, CMD_PING_RESPONSE, NULL, 0);
            return true;

        case CMD_PING_RESPONSE:
            *pos += sz;
            return true;

        default:
            /* Unknown command — skip */
            *pos += sz;
            return true;
    }
    return true;
}

static void rb_process_commands(struct libra_rollback *rb)
{
    size_t pos = 0;
    while (rb_dispatch_cmd(rb, &pos))
        ;

    if (pos > 0 && pos < rb->recv_len) {
        memmove(rb->recv_buf, rb->recv_buf + pos, rb->recv_len - pos);
        rb->recv_len -= pos;
    } else if (pos >= rb->recv_len) {
        rb->recv_len = 0;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

struct libra_rollback *libra_rollback_alloc(struct libra_ctx *ctx)
{
    struct libra_rollback *rb = (struct libra_rollback *)calloc(1, sizeof(*rb));
    if (!rb) return NULL;
    rb->ctx       = ctx;
    rb->peer_fd   = -1;
    rb->listen_fd = -1;
    rb->msg_ud    = ctx->config.userdata;
    rb->crc_check_interval = RB_CRC_INTERVAL;
    rb->local_words  = 1; /* joypad = 1 word */
    rb->remote_words = 1;
    strncpy(rb->our_nick, "libra", NP_NICK_LEN - 1);
    return rb;
}

void libra_rollback_free(struct libra_rollback *rb)
{
    if (!rb) return;
    libra_rb_disconnect(rb);

    /* Free state buffers in ring */
    for (int i = 0; i < RB_BUFFER_SIZE; i++)
        free(rb->buffer[i].state);

    free(rb->compress_buf);
    free(rb->delta_ref);
    free(rb);
}

bool libra_rb_host(struct libra_rollback *rb, uint16_t port,
                   libra_net_message_cb_t msg_cb)
{
    if (!rb || !rb->ctx->core || !rb->ctx->game_loaded) return false;
    libra_rb_disconnect(rb);

    rb->msg_cb  = msg_cb;
    rb->is_host = true;
    rb->client_id = 0;
    rb->self_frame = 0;
    rb->peer_frame = 0;
    rb->port = port ? port : NP_DEFAULT_PORT;
    rb->local_port  = 0;
    rb->remote_port = 1;
    rb->state_alloc_size = libra_serialize_size(rb->ctx);

    /* Create listening socket (dual-stack IPv4+IPv6) */
    int fd = netsock_tcp_listen(rb->port);
    if (fd < 0) {
        rb_msgf(rb, "Failed to bind port %u", rb->port);
        return false;
    }

    netsock_set_nonblocking(fd);
    rb->listen_fd = fd;

    rb_msgf(rb, "Hosting on port %u...", rb->port);
    return true;
}

/* Host: accept a single peer (called from rb_run) */
static bool rb_accept_peer(struct libra_rollback *rb)
{
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    char peer_nick[NP_NICK_LEN];
    uint32_t proto;
    int fd;
    void *raw = NULL;
    struct rb_compress_job job;
    pthread_t comp_thread;
    bool thread_started = false;

    if (rb->peer_fd >= 0 || rb->listen_fd < 0)
        return false;

    fd = accept(rb->listen_fd, (struct sockaddr *)&addr, &alen);
    if (fd < 0) return false;

    netsock_set_nodelay(fd);

    /* Serialize current state immediately so compression can run concurrently
     * with the blocking handshake exchange below. */
    memset(&job, 0, sizeof(job));
    {
        size_t sz = libra_serialize_size(rb->ctx);
        raw = sz ? malloc(sz) : NULL;
        if (raw) {
            libra_environment_set_ctx(rb->ctx);
            if (libra_serialize(rb->ctx, raw, sz)) {
                job.input      = raw;
                job.input_size = sz;
                if (pthread_create(&comp_thread, NULL, rb_compress_worker, &job) == 0)
                    thread_started = true;
            }
        }
    }

    /* Handshake (blocking) — runs concurrently with compression thread */
    memset(peer_nick, 0, sizeof(peer_nick));
    if (!rb_recv_header(rb, fd, &proto))              goto fail;
    if (!rb_send_header(rb, fd, proto))               goto fail;
    rb->delta_enabled = (rb->peer_caps & RB_CAP_DELTA_SAVESTATES) != 0;
    if (!rb_recv_nick(rb, fd, peer_nick))             goto fail;
    if (!rb_send_nick(rb, fd))                        goto fail;
    if (!rb_send_info(rb, fd))                        goto fail;
    if (!rb_recv_info(rb, fd))                        goto fail;
    if (!rb_send_sync(rb, fd, 1, peer_nick))          goto fail;
    if (!rb_recv_play(rb, fd))                        goto fail;
    if (!rb_send_mode_accepted(rb, fd, 1, peer_nick)) goto fail;

    rb->peer_fd = fd;
    rb->connected = true;
    rb->protocol = proto;

    /* Close listen socket (1v1 only) */
    close(rb->listen_fd);
    rb->listen_fd = -1;

    /* Join compression thread, then send initial savestate */
    if (thread_started) pthread_join(comp_thread, NULL);

    if (thread_started && job.result == Z_OK) {
        rb_send_precompressed(rb, rb->self_frame,
                              job.input, job.input_size,
                              job.output, job.output_size, job.crc);
    } else {
        /* Fallback: serialize+compress inline (compression failed or skipped) */
        rb_send_savestate(rb, rb->self_frame);
    }
    free(job.output);
    free(raw);

    /* Switch to non-blocking for data phase */
    netsock_set_nonblocking(fd);

    rb_msgf(rb, "Player 2 connected: %s", peer_nick);
    return true;

fail:
    if (thread_started) pthread_join(comp_thread, NULL);
    free(job.output);
    free(raw);
    close(fd);
    return false;
}

bool libra_rb_join(struct libra_rollback *rb, const char *host_ip,
                   uint16_t port, libra_net_message_cb_t msg_cb)
{
    if (!rb || !rb->ctx->core || !rb->ctx->game_loaded || !host_ip) return false;
    libra_rb_disconnect(rb);

    rb->msg_cb  = msg_cb;
    rb->is_host = false;
    rb->port = port ? port : NP_DEFAULT_PORT;
    rb->local_port  = 1;
    rb->remote_port = 0;
    rb->state_alloc_size = libra_serialize_size(rb->ctx);

    rb_msgf(rb, "Connecting to %s:%u...", host_ip, rb->port);

    int fd = netsock_tcp_connect(host_ip, rb->port);
    if (fd < 0) {
        rb_msg(rb, "Connection failed");
        return false;
    }

    /* Client handshake */
    if (!rb_send_header(rb, fd, NP_PROTO_LO)) { close(fd); return false; }

    uint32_t proto;
    if (!rb_recv_header(rb, fd, &proto)) { close(fd); return false; }
    rb->delta_enabled = (rb->peer_caps & RB_CAP_DELTA_SAVESTATES) != 0;

    if (!rb_send_nick(rb, fd)) { close(fd); return false; }

    char host_nick[NP_NICK_LEN] = {0};
    if (!rb_recv_nick(rb, fd, host_nick)) { close(fd); return false; }

    if (!rb_recv_info(rb, fd)) { close(fd); return false; }
    if (!rb_send_info(rb, fd)) { close(fd); return false; }

    if (!rb_recv_sync(rb, fd))  { close(fd); return false; }
    if (!rb_send_play(rb, fd))  { close(fd); return false; }
    if (!rb_recv_mode(rb, fd))  { close(fd); return false; }

    rb->peer_fd = fd;
    rb->connected = true;
    rb->protocol = proto;

    /* Receive initial savestate from host (blocking, 10-second timeout).
     * This ensures we start with the correct core state. */
    {
        uint32_t ss_hdr[2];
        if (!netsock_read_all(fd, ss_hdr, 8, 10000)) {
            rb_msg(rb, "Timeout waiting for host savestate");
            /* Non-fatal: proceed without it (will desync-recover later) */
            goto join_done;
        }

        uint32_t cmd = ntohl(ss_hdr[0]);
        uint32_t sz  = ntohl(ss_hdr[1]);

        if (cmd == CMD_LOAD_SAVESTATE && sz >= 12) {
            uint32_t frame_n, orig_n, crc_n;
            if (!netsock_read_all(fd, &frame_n, 4, 10000)) goto join_done;
            if (!netsock_read_all(fd, &orig_n, 4, 10000))  goto join_done;
            if (!netsock_read_all(fd, &crc_n, 4, 10000))   goto join_done;

            uint32_t frame     = ntohl(frame_n);
            uint32_t orig_size = ntohl(orig_n);
            uint32_t comp_size = sz - 12;

            void *comp_data = malloc(comp_size);
            if (comp_data && netsock_read_all(fd, comp_data, comp_size, 10000)) {
                void *state = malloc(orig_size);
                if (state) {
                    uLongf dest_len = orig_size;
                    if (uncompress((Bytef *)state, &dest_len,
                                   (const Bytef *)comp_data, comp_size) == Z_OK) {
                        /* Seed delta reference so subsequent deltas apply correctly */
                        rb_update_delta_ref(rb, state, (size_t)dest_len);
                        /* Check for NETPLAY1 container */
                        size_t inner_size;
                        const void *inner = netplay1_unwrap(state, dest_len, &inner_size);
                        if (inner) {
                            libra_environment_set_ctx(rb->ctx);
                            libra_unserialize(rb->ctx, inner, inner_size);
                        } else {
                            libra_environment_set_ctx(rb->ctx);
                            libra_unserialize(rb->ctx, state, dest_len);
                        }
                        rb->self_frame = frame;
                        rb->peer_frame = frame;
                        rb_msgf(rb, "Loaded host state at frame %u", frame);
                    }
                    free(state);
                }
            }
            free(comp_data);
        } else {
            /* Not a savestate — skip payload (might be another command) */
            uint8_t skip[256];
            uint32_t remaining = sz;
            while (remaining > 0) {
                uint32_t chunk = remaining > sizeof(skip) ? sizeof(skip) : remaining;
                if (!netsock_read_all(fd, skip, chunk, 5000)) break;
                remaining -= chunk;
            }
        }
    }

join_done:
    /* Switch to non-blocking for data phase */
    netsock_set_nonblocking(fd);

    rb_msgf(rb, "Connected to host as Player %u", rb->client_id);
    return true;
}

/* -------------------------------------------------------------------------
 * Per-frame execution
 * ---------------------------------------------------------------------- */

bool libra_rb_run(struct libra_rollback *rb)
{
    if (!rb || !rb->ctx) return false;
    libra_ctx_t *ctx = rb->ctx;

    libra_environment_set_ctx(ctx);

    /* Host: try accepting a peer if none connected yet */
    if (rb->is_host && !rb->connected)
        rb_accept_peer(rb);

    if (!rb->connected) {
        /* Not connected yet — just run locally */
        libra_run(ctx);
        return true;
    }

    /* 1. Read network data */
    if (!rb_recv_data(rb)) {
        rb_msg(rb, "Connection lost");
        return false;
    }

    /* 2. Parse commands */
    rb_process_commands(rb);

    if (!rb->connected) {
        rb_msg(rb, "Peer disconnected");
        return false;
    }

    /* 3. Check stall */
    if (rb->stall_frames > 0) {
        rb->stall_frames--;
        rb_send_noinput(rb, rb->self_frame);
        /* Still run core for one frame to keep things responsive */
        libra_run(ctx);
        return true;
    }

    /* Check if we're too far ahead of peer (stall prevention) */
    if (rb->self_frame > rb->peer_frame + RB_BUFFER_SIZE / 2) {
        /* We're way ahead — wait for peer to catch up */
        rb_send_noinput(rb, rb->self_frame);
        libra_run(ctx);
        return true;
    }

    /* 4. Capture local input */
    if (ctx->config.input_poll)
        ctx->config.input_poll(ctx->config.userdata);

    uint32_t local_input[16] = {0};
    if (ctx->config.input_state) {
        for (unsigned p = 0; p < 16; p++) {
            unsigned dev = (p < ctx->port_count) ? ctx->port_devices[p] : 0;
            if ((dev & 0xFF) == 1 /* JOYPAD */ || dev == 0) {
                /* Build bitmask from individual button queries */
                uint32_t mask = 0;
                for (unsigned btn = 0; btn <= 15; btn++) {
                    if (ctx->config.input_state(ctx->config.userdata,
                                                p, 1 /*JOYPAD*/, 0, btn))
                        mask |= (1u << btn);
                }
                local_input[p] = mask;
            }
        }
    }

    /* Apply input latency: store input at self_frame + latency */
    uint32_t input_frame = rb->self_frame + rb->input_latency;
    struct rb_frame *cur = rb_slot(rb, input_frame);
    cur->frame_num = input_frame;
    memcpy(cur->local_input, local_input, sizeof(local_input));
    cur->have_local = true;

    /* 5. Send INPUT to peer */
    rb_send_input(rb, input_frame, local_input, rb->local_words);

    /* 6. Detect rollback needed:
     * If the peer sent input for a frame we already ran,
     * AND the real input differs from what we predicted */
    bool need_rollback = false;
    uint32_t rollback_to = rb->self_frame;

    /* Check frames between peer_frame and self_frame that got new data */
    for (uint32_t f = rb->peer_frame; f < rb->self_frame; f++) {
        struct rb_frame *slot = rb_slot(rb, f);
        if (slot->have_remote && slot->frame_num == f) {
            /* Check if we have a saved state at this frame */
            if (slot->state) {
                need_rollback = true;
                if (f < rollback_to)
                    rollback_to = f;
            }
        }
    }

    /* 7. Rollback if needed */
    if (need_rollback && rollback_to < rb->self_frame) {
        struct rb_frame *target = rb_slot(rb, rollback_to);
        if (target->state && target->state_size > 0) {
            rb->is_replay = true;
            ctx->savestate_context = RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;

            /* Load state from the rollback point */
            libra_unserialize(ctx, target->state, target->state_size);

            /* Save original callbacks */
            libra_video_cb_t orig_video = ctx->config.video;
            libra_audio_cb_t orig_audio = ctx->config.audio;

            /* Mute audio/video during replay */
            ctx->config.video = NULL;
            ctx->config.audio = NULL;

            /* Replay from rollback_to to self_frame - 1 */
            ctx->input_override_active = true;
            for (uint32_t f = rollback_to; f < rb->self_frame; f++) {
                struct rb_frame *slot = rb_slot(rb, f);

                /* Set up input override with resolved input */
                memset(ctx->input_override, 0, sizeof(ctx->input_override));
                ctx->input_override[rb->local_port] = slot->local_input[rb->local_port];
                if (slot->have_remote)
                    ctx->input_override[rb->remote_port] = slot->remote_input[rb->remote_port];
                else if (f > 0) {
                    /* Predict: repeat last known remote input */
                    struct rb_frame *prev = rb_slot(rb, f - 1);
                    ctx->input_override[rb->remote_port] = prev->remote_input[rb->remote_port];
                }

                libra_run(ctx);

                /* Save state for this replayed frame */
                rb_ensure_state_buf(rb, slot);
                if (slot->state)
                    libra_serialize(ctx, slot->state, slot->state_size);
            }
            ctx->input_override_active = false;

            /* Restore callbacks */
            ctx->config.video = orig_video;
            ctx->config.audio = orig_audio;

            ctx->savestate_context = RETRO_SAVESTATE_CONTEXT_NORMAL;
            rb->is_replay = false;
        }
    }

    /* 8. Save state for current frame */
    struct rb_frame *current = rb_slot(rb, rb->self_frame);
    current->frame_num = rb->self_frame;
    rb_ensure_state_buf(rb, current);
    if (current->state)
        libra_serialize(ctx, current->state, current->state_size);

    /* 9. Set input for current frame */
    /* Get local input for this frame (may be from latency-delayed slot) */
    struct rb_frame *input_slot = rb_slot(rb, rb->self_frame);

    ctx->input_override_active = true;
    memset(ctx->input_override, 0, sizeof(ctx->input_override));
    ctx->input_override[rb->local_port] = input_slot->local_input[rb->local_port];

    if (input_slot->have_remote) {
        ctx->input_override[rb->remote_port] = input_slot->remote_input[rb->remote_port];
    } else if (rb->self_frame > 0) {
        /* Predict: repeat last known remote input */
        struct rb_frame *prev = rb_slot(rb, rb->self_frame - 1);
        ctx->input_override[rb->remote_port] = prev->remote_input[rb->remote_port];
    }

    /* 10. Run current frame */
    libra_run(ctx);

    ctx->input_override_active = false;

    /* 11. CRC check periodically */
    if (rb->crc_check_interval > 0 &&
        rb->self_frame % rb->crc_check_interval == 0 &&
        current->state) {
        current->crc = (uint32_t)crc32(0L, (const Bytef *)current->state,
                                        (uInt)current->state_size);
        rb_send_crc(rb, rb->self_frame, current->crc);
    }

    /* 12. Send pending savestate (host responding to desync) */
    if (rb->savestate_pending) {
        rb_send_savestate(rb, rb->self_frame);
        rb->savestate_pending = false;
    }

    /* 13. SRAM dirty check */
    /* Let the caller handle SRAM saves — we don't touch it here */

    /* 14. Advance frame counter */
    rb->self_frame++;

    return true;
}

void libra_rb_disconnect(struct libra_rollback *rb)
{
    if (!rb) return;

    if (rb->peer_fd >= 0) {
        netsock_write_cmd(rb->peer_fd, CMD_DISCONNECT, NULL, 0);
        close(rb->peer_fd);
        rb->peer_fd = -1;
    }

    if (rb->listen_fd >= 0) {
        close(rb->listen_fd);
        rb->listen_fd = -1;
    }

    rb->connected = false;
    rb->recv_len = 0;
    rb->stall_frames = 0;
    rb->savestate_pending = false;

    /* Restore input override */
    if (rb->ctx) {
        rb->ctx->input_override_active = false;
        memset(rb->ctx->input_override, 0, sizeof(rb->ctx->input_override));
    }
}

bool libra_rb_active(const struct libra_rollback *rb)
{
    if (!rb) return false;
    return rb->listen_fd >= 0 || rb->connected;
}

bool libra_rb_connected(const struct libra_rollback *rb)
{
    if (!rb) return false;
    return rb->connected;
}

bool libra_rb_is_host(const struct libra_rollback *rb)
{
    if (!rb) return false;
    return rb->is_host;
}

bool libra_rb_is_replay(const struct libra_rollback *rb)
{
    if (!rb) return false;
    return rb->is_replay;
}

void libra_rb_set_input_latency(struct libra_rollback *rb, unsigned frames)
{
    if (rb) {
        if (frames > 10) frames = 10;
        rb->input_latency = frames;
    }
}

unsigned libra_rb_input_latency(const struct libra_rollback *rb)
{
    return rb ? rb->input_latency : 0;
}

/* -------------------------------------------------------------------------
 * Relay host/join (rollback mode over MITM tunnel)
 * ---------------------------------------------------------------------- */

bool libra_rb_host_relay(struct libra_rollback *rb,
                          const char *relay_ip, uint16_t relay_port,
                          uint8_t mitm_id_out[MITM_ID_SIZE],
                          libra_net_message_cb_t msg_cb)
{
    uint32_t proto;
    char peer_nick[NP_NICK_LEN];
    int fd;
    void *raw = NULL;
    struct rb_compress_job job;
    pthread_t comp_thread;
    bool thread_started = false;

    if (!rb || !rb->ctx->core || !rb->ctx->game_loaded || !relay_ip) return false;
    libra_rb_disconnect(rb);

    rb->msg_cb  = msg_cb;
    rb->is_host = true;
    rb->client_id = 0;
    rb->self_frame = 0;
    rb->peer_frame = 0;
    rb->local_port  = 0;
    rb->remote_port = 1;
    rb->state_alloc_size = libra_serialize_size(rb->ctx);

    uint16_t port = relay_port ? relay_port : NP_DEFAULT_PORT;
    rb_msgf(rb, "Connecting to relay %s:%u...", relay_ip, port);

    fd = rb_relay_create_session(rb, relay_ip, port, mitm_id_out);
    if (fd < 0) return false;

    rb_msg(rb, "Relay session created, waiting for peer...");

    /* Serialize current state immediately so compression runs while we wait
     * for the peer to connect and complete the handshake exchange. */
    memset(&job, 0, sizeof(job));
    {
        size_t sz = libra_serialize_size(rb->ctx);
        raw = sz ? malloc(sz) : NULL;
        if (raw) {
            libra_environment_set_ctx(rb->ctx);
            if (libra_serialize(rb->ctx, raw, sz)) {
                job.input      = raw;
                job.input_size = sz;
                if (pthread_create(&comp_thread, NULL, rb_compress_worker, &job) == 0)
                    thread_started = true;
            }
        }
    }

    /* Server-side handshake (blocking) — runs concurrently with compression */
    memset(peer_nick, 0, sizeof(peer_nick));
    if (!rb_recv_header(rb, fd, &proto))              goto fail;
    if (!rb_send_header(rb, fd, proto))               goto fail;
    rb->delta_enabled = (rb->peer_caps & RB_CAP_DELTA_SAVESTATES) != 0;
    if (!rb_recv_nick(rb, fd, peer_nick))             goto fail;
    if (!rb_send_nick(rb, fd))                        goto fail;
    if (!rb_send_info(rb, fd))                        goto fail;
    if (!rb_recv_info(rb, fd))                        goto fail;
    if (!rb_send_sync(rb, fd, 1, peer_nick))          goto fail;
    if (!rb_recv_play(rb, fd))                        goto fail;
    if (!rb_send_mode_accepted(rb, fd, 1, peer_nick)) goto fail;

    rb->peer_fd = fd;
    rb->connected = true;
    rb->protocol = proto;

    /* Join compression thread, then send initial savestate */
    if (thread_started) pthread_join(comp_thread, NULL);

    if (thread_started && job.result == Z_OK) {
        rb_send_precompressed(rb, rb->self_frame,
                              job.input, job.input_size,
                              job.output, job.output_size, job.crc);
    } else {
        rb_send_savestate(rb, rb->self_frame);
    }
    free(job.output);
    free(raw);

    netsock_set_nonblocking(fd);

    rb_msgf(rb, "Relay: peer connected: %s", peer_nick);
    return true;

fail:
    if (thread_started) pthread_join(comp_thread, NULL);
    free(job.output);
    free(raw);
    close(fd);
    return false;
}

bool libra_rb_join_relay(struct libra_rollback *rb,
                          const char *relay_ip, uint16_t relay_port,
                          const uint8_t mitm_id[MITM_ID_SIZE],
                          libra_net_message_cb_t msg_cb)
{
    if (!rb || !rb->ctx->core || !rb->ctx->game_loaded || !relay_ip || !mitm_id)
        return false;
    libra_rb_disconnect(rb);

    rb->msg_cb  = msg_cb;
    rb->is_host = false;
    rb->local_port  = 1;
    rb->remote_port = 0;
    rb->state_alloc_size = libra_serialize_size(rb->ctx);

    uint16_t port = relay_port ? relay_port : NP_DEFAULT_PORT;
    rb_msgf(rb, "Joining relay %s:%u...", relay_ip, port);

    int fd = rb_relay_join_session(rb, relay_ip, port, mitm_id);
    if (fd < 0) return false;

    /* Client handshake over relay tunnel */
    if (!rb_send_header(rb, fd, NP_PROTO_LO)) { close(fd); return false; }
    uint32_t proto;
    if (!rb_recv_header(rb, fd, &proto))       { close(fd); return false; }
    rb->delta_enabled = (rb->peer_caps & RB_CAP_DELTA_SAVESTATES) != 0;
    if (!rb_send_nick(rb, fd))                 { close(fd); return false; }
    char host_nick[NP_NICK_LEN] = {0};
    if (!rb_recv_nick(rb, fd, host_nick))      { close(fd); return false; }
    if (!rb_recv_info(rb, fd))                 { close(fd); return false; }
    if (!rb_send_info(rb, fd))                 { close(fd); return false; }
    if (!rb_recv_sync(rb, fd))                 { close(fd); return false; }
    if (!rb_send_play(rb, fd))                 { close(fd); return false; }
    if (!rb_recv_mode(rb, fd))                 { close(fd); return false; }

    rb->peer_fd = fd;
    rb->connected = true;
    rb->protocol = proto;

    /* Receive initial savestate from host (blocking) */
    {
        uint32_t ss_hdr[2];
        if (netsock_read_all(fd, ss_hdr, 8, 10000)) {
            uint32_t cmd = ntohl(ss_hdr[0]);
            uint32_t sz  = ntohl(ss_hdr[1]);
            if (cmd == CMD_LOAD_SAVESTATE && sz >= 12) {
                uint32_t frame_n, orig_n, crc_n;
                if (netsock_read_all(fd, &frame_n, 4, 10000) &&
                    netsock_read_all(fd, &orig_n, 4, 10000) &&
                    netsock_read_all(fd, &crc_n, 4, 10000)) {
                    uint32_t frame     = ntohl(frame_n);
                    uint32_t orig_size = ntohl(orig_n);
                    uint32_t comp_size = sz - 12;
                    void *comp_data = malloc(comp_size);
                    if (comp_data && netsock_read_all(fd, comp_data, comp_size, 10000)) {
                        void *state = malloc(orig_size);
                        if (state) {
                            uLongf dest_len = orig_size;
                            if (uncompress((Bytef *)state, &dest_len,
                                           (const Bytef *)comp_data, comp_size) == Z_OK) {
                                rb_update_delta_ref(rb, state, (size_t)dest_len);
                                size_t inner_size;
                                const void *inner = netplay1_unwrap(state, dest_len, &inner_size);
                                libra_environment_set_ctx(rb->ctx);
                                if (inner)
                                    libra_unserialize(rb->ctx, inner, inner_size);
                                else
                                    libra_unserialize(rb->ctx, state, dest_len);
                                rb->self_frame = frame;
                                rb->peer_frame = frame;
                                rb_msgf(rb, "Loaded host state at frame %u", frame);
                            }
                            free(state);
                        }
                    }
                    free(comp_data);
                }
            } else {
                /* Skip unknown command payload */
                uint8_t skip[256];
                uint32_t remaining = sz;
                while (remaining > 0) {
                    uint32_t chunk = remaining > sizeof(skip) ? sizeof(skip) : remaining;
                    if (!netsock_read_all(fd, skip, chunk, 5000)) break;
                    remaining -= chunk;
                }
            }
        }
    }

    netsock_set_nonblocking(fd);
    rb_msgf(rb, "Relay: connected to host as Player %u", rb->client_id);
    return true;
}
