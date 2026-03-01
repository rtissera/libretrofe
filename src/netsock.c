// SPDX-License-Identifier: MIT
/*
 * libra netsock — shared TCP helpers for netplay and rollback
 *
 * Extracted from netplay.c to be reused by rollback.c.
 * No GPL code; only POSIX sockets and interop constants.
 */
#include "netsock.h"

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
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
