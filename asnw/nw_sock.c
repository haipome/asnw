/*
 * Description: address type, address parsing and socket option helpers
 *     History: yang@haipo.me, 2016/03/16, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <strings.h>
# include <errno.h>
# include <fcntl.h>
# include <unistd.h>
# include <stdbool.h>

# include "nw_sock.h"

char *nw_sock_human_addr(nw_addr *addr)
{
    static __thread char str[NW_SOCK_HUMAN_ADDR_SIZE];
    return nw_sock_human_addr_s(addr, str);
}

char *nw_sock_human_addr_s(nw_addr *addr, char *dest)
{
    char ip[INET6_ADDRSTRLEN];

    switch (addr->family) {
    case AF_INET:
        inet_ntop(addr->family, &addr->in.sin_addr, ip, sizeof(ip));
        snprintf(dest, NW_SOCK_HUMAN_ADDR_SIZE, "%s:%u", ip, ntohs(addr->in.sin_port));
        break;
    case AF_INET6:
        inet_ntop(addr->family, &addr->in6.sin6_addr, ip, sizeof(ip));
        snprintf(dest, NW_SOCK_HUMAN_ADDR_SIZE, "[%s]:%u", ip, ntohs(addr->in6.sin6_port));
        break;
    case AF_UNIX:
        snprintf(dest, NW_SOCK_HUMAN_ADDR_SIZE, "%s:%s", "unix", (addr->un.sun_path));
        break;
    case NW_SOCK_AF_NAME:
        snprintf(dest, NW_SOCK_HUMAN_ADDR_SIZE, "%s:%u", addr->host.name, addr->host.port);
        break;
    default:
        dest[0] = 0;
        break;
    }

    return dest;
}

char *nw_sock_ip(nw_addr *addr)
{
    static __thread char ip[NW_SOCK_IP_SIZE];
    return nw_sock_ip_s(addr, ip);
}

char *nw_sock_ip_s(nw_addr *addr, char *ip)
{
    switch (addr->family) {
    case AF_INET:
        inet_ntop(addr->family, &addr->in.sin_addr, ip, NW_SOCK_IP_SIZE);
        break;
    case AF_INET6:
        inet_ntop(addr->family, &addr->in6.sin6_addr, ip, NW_SOCK_IP_SIZE);
        break;
    default:
        ip[0] = 0;
        break;
    }
    return ip;
}

int nw_sock_set_mode(nw_addr *addr, mode_t mode)
{
    if (addr->family != AF_UNIX)
        return 0;
    return chmod(addr->un.sun_path, mode);
}

int nw_sock_peer_addr(int sockfd, nw_addr *addr)
{
    addr->addrlen = sizeof(addr->un);
    if (getpeername(sockfd, NW_SOCKADDR(addr), &addr->addrlen) == 0) {
        addr->family = NW_SOCKADDR(addr)->sa_family;
        return 0;
    }
    return -1;
}

int nw_sock_local_addr(int sockfd, nw_addr *addr)
{
    addr->addrlen = sizeof(addr->un);
    if (getsockname(sockfd, NW_SOCKADDR(addr), &addr->addrlen) == 0) {
        addr->family = NW_SOCKADDR(addr)->sa_family;
        return 0;
    }
    return -1;
}

static int addr_fill_inet(nw_addr *addr, const char *host, const char *port)
{
    /* the port must be a plain decimal number in 0..65535 */
    if (*port < '0' || *port > '9')
        return -1;
    char *end = NULL;
    unsigned long port_num = strtoul(port, &end, 10);
    if (*end != '\0' || port_num > 65535)
        return -1;

    memset(addr, 0, sizeof(nw_addr));
    if (strchr(host, ':') == NULL) {
        addr->in.sin_family = AF_INET;
        if (inet_pton(addr->in.sin_family, host, &addr->in.sin_addr) <= 0)
            return -1;
        addr->in.sin_port = htons(port_num);
        addr->family = addr->in.sin_family;
        addr->addrlen = sizeof(addr->in);
    } else {
        addr->in6.sin6_family = AF_INET6;
        if (inet_pton(addr->in6.sin6_family, host, &addr->in6.sin6_addr) <= 0)
            return -1;
        addr->in6.sin6_port = htons(port_num);
        addr->family = addr->in6.sin6_family;
        addr->addrlen = sizeof(addr->in6);
    }

    return 0;
}

/* not an ip: the name of a host, if it can be one. 0, -4 for what is
 * rather an ip or a port gone wrong, -6 for what is neither */
static int addr_fill_name(nw_addr *addr, const char *host, const char *port)
{
    if (*port < '0' || *port > '9')
        return -4;
    char *end = NULL;
    unsigned long port_num = strtoul(port, &end, 10);
    if (*end != '\0' || port_num > 65535)
        return -4;
    /* an ipv6 address that did not parse stays a bad ip */
    if (strchr(host, ':') != NULL)
        return -4;

    size_t len = strlen(host);
    if (len == 0 || len >= NW_SOCK_MAX_NAME)
        return -6;
    bool only_digits_and_dots = true;
    for (size_t i = 0; i < len; ++i) {
        char c = host[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != '-' && c != '_' && c != '.')
            return -6;
        if (!digit && c != '.')
            only_digits_and_dots = false;
    }
    /* 1.2.3 or 300.1.1.1: a mistyped ip, not a name */
    if (only_digits_and_dots)
        return -4;

    memset(addr, 0, sizeof(nw_addr));
    addr->family = NW_SOCK_AF_NAME;
    addr->host.port = port_num;
    memcpy(addr->host.name, host, len);
    return 0;
}

static int addr_fill_unix(nw_addr *addr, const char *unix_path)
{
    size_t pathlen = strlen(unix_path);
    if (pathlen == 0 || pathlen >= sizeof(addr->un.sun_path))
        return -1;
    memset(addr, 0, sizeof(nw_addr));
    addr->un.sun_family = AF_UNIX;
    strcpy(addr->un.sun_path, unix_path);
    addr->family = addr->un.sun_family;
    addr->addrlen = sizeof(addr->un);

    return 0;
}

int nw_sock_addr_parse(const char *str, nw_addr *addr, int *sock_type)
{
    char *s = strdup(str);
    if (s == NULL)
        return -1;
    char *sep = strchr(s, '@');
    if (sep == NULL) {
        free(s);
        return -1;
    }
    *sep = '\0';
    char *type = s;
    char *name = sep + 1;
    int is_inet = 0;

    if (strcasecmp(type, "tcp") == 0) {
        *sock_type = SOCK_STREAM;
        is_inet = 1;
    } else if (strcasecmp(type, "udp") == 0) {
        *sock_type = SOCK_DGRAM;
        is_inet = 1;
    } else if (strcasecmp(type, "stream") == 0) {
        *sock_type = SOCK_STREAM;
    } else if (strcasecmp(type, "dgram") == 0) {
        *sock_type = SOCK_DGRAM;
    } else if (strcasecmp(type, "seqpacket") == 0) {
        *sock_type = SOCK_SEQPACKET;
    } else {
        free(s);
        return -2;
    }

    if (is_inet) {
        /* ipv4:port, [ipv6]:port or ipv6:port. the port follows the LAST
         * colon, an ipv6 address is full of them */
        char *host = name;
        bool bracketed = (name[0] == '[');
        if (bracketed) {
            host = name + 1;
            sep = strchr(host, ']');
            if (sep == NULL || sep[1] != ':') {
                free(s);
                return -3;
            }
            *sep++ = '\0';
        } else {
            sep = strrchr(name, ':');
            if (sep == NULL) {
                free(s);
                return -3;
            }
        }
        *sep = '\0';
        char *port = sep + 1;
        if (addr_fill_inet(addr, host, port) < 0) {
            /* [..] can only be an ipv6 address */
            int ret = bracketed ? -4 : addr_fill_name(addr, host, port);
            if (ret < 0) {
                free(s);
                return ret;
            }
        }
    } else {
        if (addr_fill_unix(addr, name) < 0) {
            free(s);
            return -5;
        }
    }

    free(s);
    return 0;
}

int nw_sock_errno(int sockfd)
{
    int _errno = 0;
    socklen_t len = sizeof(int);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &_errno, &len) == 0)
        return _errno;
    if (errno != 0)
        return errno;
    return EBADFD;
}

int nw_sock_get_send_buf(int sockfd, int *buf_size)
{
    socklen_t len = sizeof(*buf_size);
    if (getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, buf_size, &len) != 0)
        return -1;
    return 0;
}

int nw_sock_get_recv_buf(int sockfd, int *buf_size)
{
    socklen_t len = sizeof(*buf_size);
    if (getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, buf_size, &len) != 0)
        return -1;
    return 0;
}

int nw_sock_set_send_buf(int sockfd, int buf_size)
{
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) != 0)
        return -1;
    return 0;
}

int nw_sock_set_recv_buf(int sockfd, int buf_size)
{
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) != 0)
        return -1;
    return 0;
}

int nw_sock_set_nonblock(int sockfd)
{
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) != 0)
        return -1;
    return 0;
}

int nw_sock_set_cloexec(int sockfd)
{
    int flags = fcntl(sockfd, F_GETFD, 0);
    if (flags < 0)
        return -1;
    if (fcntl(sockfd, F_SETFD, flags | FD_CLOEXEC) != 0)
        return -1;
    return 0;
}

int nw_sock_set_no_delay(int sockfd)
{
    int val = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) != 0)
        return -1;
    return 0;
}

int nw_sock_set_reuse_addr(int sockfd)
{
    int val = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) != 0)
        return -1;
    return 0;
}
