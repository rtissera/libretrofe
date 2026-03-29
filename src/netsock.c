// SPDX-License-Identifier: MIT
/*
 * libra netsock — shared TCP helpers for netplay and rollback
 *
 * Extracted from netplay.c to be reused by rollback.c.
 * No GPL code; only POSIX sockets and interop constants.
 */
#include "netsock.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

bool netsock_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool netsock_set_nodelay(int fd)
{
    int flag = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      &flag, sizeof(flag)) == 0;
}

bool netsock_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR))
                continue;
            return false;
        }
        p   += n;
        len -= (size_t)n;
    }
    return true;
}

bool netsock_read_all(int fd, void *buf, size_t len, int timeout_ms)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, timeout_ms);
        if (r <= 0) return false;

        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p   += n;
        len -= (size_t)n;
    }
    return true;
}

bool netsock_write_cmd(int fd, uint32_t cmd_id,
                       const void *data, uint32_t data_len)
{
    uint32_t hdr[2];
    hdr[0] = htonl(cmd_id);
    hdr[1] = htonl(data_len);
    if (!netsock_write_all(fd, hdr, 8)) return false;
    if (data_len > 0 && !netsock_write_all(fd, data, data_len)) return false;
    return true;
}

uint32_t netsock_platform_magic(void)
{
    /* Same layout as the public spec: bit30=bigendian, 29-15=sizeof(size_t),
     * 14-0=sizeof(long).  This is interop data, not code. */
    return ((uint32_t)(1 == htonl(1)) << 30)
         | ((uint32_t)sizeof(size_t) << 15)
         | ((uint32_t)sizeof(long));
}

uint32_t netsock_impl_magic(void)
{
    /* Hash our own version string — different from RetroArch's, which is
     * fine: the protocol treats a mismatch as a warning, not an error. */
    const char *ver = "libra-0.1";
    uint32_t h = 0;
    for (size_t i = 0; ver[i]; i++)
        h = (h << 1) | ((h >> 31) & 1);
    return h;
}

/* -------------------------------------------------------------------------
 * IPv4 + IPv6 dual-stack helpers
 * ---------------------------------------------------------------------- */

int netsock_tcp_connect(const char *host, uint16_t port)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        /* Non-blocking connect with 5-second timeout */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int ret = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (ret == 0) {
            /* Immediate connect (unusual but valid) */
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            if (poll(&pfd, 1, 5000) <= 0) {
                close(fd); fd = -1; continue;
            }
            int err = 0;
            socklen_t errlen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
            if (err != 0) { close(fd); fd = -1; continue; }
        } else {
            close(fd); fd = -1; continue;
        }

        /* Connected — make blocking and set TCP_NODELAY */
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        netsock_set_nodelay(fd);
        break;
    }

    freeaddrinfo(res);
    return fd;
}

int netsock_tcp_listen(uint16_t port)
{
    int opt;

    /* Try dual-stack IPv6 first (accepts both IPv4 and IPv6) */
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd >= 0) {
        opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int no = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

        struct sockaddr_in6 addr = {0};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr   = in6addr_any;
        addr.sin6_port   = htons(port);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(fd, 16) == 0)
            return fd;

        close(fd);
    }

    /* Fallback: IPv4-only (for kernels/systems without IPv6) */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr4 = {0};
    addr4.sin_family      = AF_INET;
    addr4.sin_addr.s_addr = INADDR_ANY;
    addr4.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) == 0 &&
        listen(fd, 16) == 0)
        return fd;

    close(fd);
    return -1;
}

/* =========================================================================
 * UDP helpers (for EmuLnk companion protocol)
 * ====================================================================== */

libra_socket_t libra_socket_udp(void)
{
    return socket(AF_INET, SOCK_DGRAM, 0);
}

bool libra_socket_bind(libra_socket_t sock, int port)
{
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);
    return bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0;
}

void libra_socket_set_nonblocking_udp(libra_socket_t sock)
{
    netsock_set_nonblocking(sock);
}

int libra_socket_recvfrom(libra_socket_t sock, void *buf, int buflen,
                           void *from, void *fromlen)
{
    return (int)recvfrom(sock, buf, (size_t)buflen, 0,
                         (struct sockaddr*)from, (socklen_t*)fromlen);
}

int libra_socket_sendto(libra_socket_t sock, const void *buf, int len,
                         const void *to, unsigned tolen)
{
    return (int)sendto(sock, buf, (size_t)len, 0,
                       (const struct sockaddr*)to, (socklen_t)tolen);
}

void libra_socket_close(libra_socket_t sock)
{
    if (sock != LIBRA_INVALID_SOCKET)
        close(sock);
}
