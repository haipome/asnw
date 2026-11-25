/*
 * KNOWN BUG: nw_sock_cfg_parse() cannot parse any IPv6 address.
 * nw_sock.h says "ip can support ipv6", but the host/port split uses the
 * FIRST ':' (strchr), so "::1:8080" becomes host "" and port ":1:8080".
 * The nw_addr_t / session code is IPv6 capable; only the parser blocks it.
 */
# include "common.h"

int main(void)
{
    nw_addr_t addr;
    int type;
    int plain = nw_sock_cfg_parse("tcp@::1:8080", &addr, &type);
    int bracket = nw_sock_cfg_parse("tcp@[::1]:8080", &addr, &type);
    fprintf(stderr, "tcp@::1:8080 -> %d, tcp@[::1]:8080 -> %d\n", plain, bracket);
    CHECK(plain == 0 || bracket == 0);
    CHECK_EQ(addr.family, AF_INET6);
    CHECK_EQ(ntohs(addr.in6.sin6_port), 8080);
    return 0;
}
