// SPDX-License-Identifier: MIT
/*
 * libra relay — RA-compatible MITM relay server for NAT traversal
 *
 * Clean-room implementation.  The wire format (RATL/RATA/RATS handshake)
 * is an interoperability fact, not copied code.
 *
 * Usage: relay [--port PORT]   (default 55435)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

/* Wire constants (interop facts) */
#define MITM_LINK_MAGIC    0x5241544Cu  /* "RATL" */
#define MITM_ADDR_MAGIC    0x52415441u  /* "RATA" */
#define MITM_MAGIC         0x52415453u  /* "RATS" */
#define MITM_ID_SIZE       16

#define MAX_SESSIONS       256
#define FORWARD_BUF_SIZE   8192
#define DEFAULT_PORT       55435

enum session_state {
    SESS_FREE = 0,
    SESS_WAITING,      /* host connected, waiting for client */
    SESS_ACTIVE        /* both connected, forwarding */
};

struct session {
    enum session_state state;
    uint8_t  id[MITM_ID_SIZE];
    int      fd_host;
    int      fd_client;
};

static struct session sessions[MAX_SESSIONS];

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_nodelay(int fd)
{
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

static bool write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool read_all_timeout(int fd, void *buf, size_t len, int timeout_ms)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return false;
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void generate_id(uint8_t id[MITM_ID_SIZE])
{
    /* magic prefix */
    uint32_t magic = htonl(MITM_MAGIC);
    memcpy(id, &magic, 4);

    /* 12 random bytes */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(id + 4, 1, 12, f) != 12) {
            /* fallback: use time-based */
            uint32_t t = (uint32_t)time(NULL);
            memcpy(id + 4, &t, 4);
        }
        fclose(f);
    } else {
        uint32_t t = (uint32_t)time(NULL);
        memcpy(id + 4, &t, 4);
    }
}

static int find_session_by_id(const uint8_t id[MITM_ID_SIZE])
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state == SESS_WAITING &&
            memcmp(sessions[i].id, id, MITM_ID_SIZE) == 0)
            return i;
    }
    return -1;
}

static int alloc_session(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state == SESS_FREE)
            return i;
    }
    return -1;
}

static void free_session(int idx)
{
    struct session *s = &sessions[idx];
    if (s->fd_host >= 0)   { close(s->fd_host);   s->fd_host   = -1; }
    if (s->fd_client >= 0) { close(s->fd_client); s->fd_client = -1; }
    s->state = SESS_FREE;
}

static void id_to_hex(const uint8_t id[MITM_ID_SIZE], char out[33])
{
    for (int i = 0; i < MITM_ID_SIZE; i++)
        sprintf(out + i * 2, "%02x", id[i]);
    out[32] = '\0';
}

static void handle_new_connection(int fd)
{
    set_nodelay(fd);

    /* Read first 4 bytes to determine handshake type */
    uint32_t magic;
    if (!read_all_timeout(fd, &magic, 4, 5000)) {
        close(fd);
        return;
    }
    magic = ntohl(magic);

    if (magic == MITM_LINK_MAGIC) {
        /* Host wants to create a session */
        int idx = alloc_session();
        if (idx < 0) {
            fprintf(stderr, "relay: no free session slots\n");
            close(fd);
            return;
        }

        struct session *s = &sessions[idx];
        s->state = SESS_WAITING;
        s->fd_host = fd;
        s->fd_client = -1;
        generate_id(s->id);

        /* Reply with the session ID */
        if (!write_all(fd, s->id, MITM_ID_SIZE)) {
            free_session(idx);
            return;
        }

        set_nonblocking(fd);

        char hex[33];
        id_to_hex(s->id, hex);
        fprintf(stderr, "relay: session %d created (host fd=%d) token=%s\n",
                idx, fd, hex);

    } else if (magic == MITM_ADDR_MAGIC) {
        /* Client wants to join a session */
        uint8_t id[MITM_ID_SIZE];
        if (!read_all_timeout(fd, id, MITM_ID_SIZE, 5000)) {
            close(fd);
            return;
        }

        int idx = find_session_by_id(id);
        if (idx < 0) {
            char hex[33];
            id_to_hex(id, hex);
            fprintf(stderr, "relay: no session found for token %s\n", hex);
            close(fd);
            return;
        }

        struct session *s = &sessions[idx];
        s->fd_client = fd;
        s->state = SESS_ACTIVE;

        /* Reply with the session ID (echo) */
        if (!write_all(fd, s->id, MITM_ID_SIZE)) {
            free_session(idx);
            return;
        }

        set_nonblocking(fd);

        char hex[33];
        id_to_hex(s->id, hex);
        fprintf(stderr, "relay: session %d paired (client fd=%d) token=%s\n",
                idx, fd, hex);

    } else {
        /* Unknown handshake */
        fprintf(stderr, "relay: unknown magic 0x%08x, closing fd=%d\n", magic, fd);
        close(fd);
    }
}

static void forward_data(int from_fd, int to_fd, int session_idx)
{
    uint8_t buf[FORWARD_BUF_SIZE];
    ssize_t n = read(from_fd, buf, sizeof(buf));
    if (n > 0) {
        if (!write_all(to_fd, buf, (size_t)n)) {
            fprintf(stderr, "relay: write failed on session %d, closing\n", session_idx);
            free_session(session_idx);
        }
    } else if (n == 0) {
        /* EOF */
        fprintf(stderr, "relay: session %d closed (EOF)\n", session_idx);
        free_session(session_idx);
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            fprintf(stderr, "relay: session %d read error, closing\n", session_idx);
            free_session(session_idx);
        }
    }
}

int main(int argc, char **argv)
{
    uint16_t port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s [--port PORT]\n", argv[0]);
            return 0;
        }
    }

    /* Initialize sessions */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        sessions[i].state = SESS_FREE;
        sessions[i].fd_host = -1;
        sessions[i].fd_client = -1;
    }

    /* Create listening socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    set_nonblocking(listen_fd);
    fprintf(stderr, "relay: listening on port %u\n", port);

    /* Main loop */
    for (;;) {
        /* Build poll set: listen_fd + all active session fds */
        struct pollfd pfds[1 + MAX_SESSIONS * 2];
        int           pfd_session[1 + MAX_SESSIONS * 2]; /* session index */
        int           pfd_is_host[1 + MAX_SESSIONS * 2]; /* 1=host fd, 0=client fd */
        int           nfds = 0;

        pfds[nfds].fd = listen_fd;
        pfds[nfds].events = POLLIN;
        pfd_session[nfds] = -1;
        nfds++;

        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (sessions[i].state == SESS_WAITING && sessions[i].fd_host >= 0) {
                pfds[nfds].fd = sessions[i].fd_host;
                pfds[nfds].events = POLLIN;
                pfd_session[nfds] = i;
                pfd_is_host[nfds] = 1;
                nfds++;
            }
            if (sessions[i].state == SESS_ACTIVE) {
                if (sessions[i].fd_host >= 0) {
                    pfds[nfds].fd = sessions[i].fd_host;
                    pfds[nfds].events = POLLIN;
                    pfd_session[nfds] = i;
                    pfd_is_host[nfds] = 1;
                    nfds++;
                }
                if (sessions[i].fd_client >= 0) {
                    pfds[nfds].fd = sessions[i].fd_client;
                    pfds[nfds].events = POLLIN;
                    pfd_session[nfds] = i;
                    pfd_is_host[nfds] = 0;
                    nfds++;
                }
            }
        }

        int ret = poll(pfds, (nfds_t)nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (ret == 0) continue;

        for (int i = 0; i < nfds; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;

            if (pfd_session[i] < 0) {
                /* Listen socket: accept new connection */
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int new_fd = accept(listen_fd,
                                    (struct sockaddr *)&client_addr, &client_len);
                if (new_fd >= 0)
                    handle_new_connection(new_fd);
            } else {
                int si = pfd_session[i];
                if (sessions[si].state == SESS_WAITING) {
                    /* Host disconnected while waiting */
                    fprintf(stderr, "relay: session %d host disconnected while waiting\n", si);
                    free_session(si);
                } else if (sessions[si].state == SESS_ACTIVE) {
                    if (pfd_is_host[i])
                        forward_data(sessions[si].fd_host,
                                     sessions[si].fd_client, si);
                    else
                        forward_data(sessions[si].fd_client,
                                     sessions[si].fd_host, si);
                }
            }
        }
    }

    close(listen_fd);
    return 0;
}
