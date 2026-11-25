/*
 * KNOWN BUG: a unix datagram server truncates peer addresses.
 * recvfrom() is called with &ses->peer_addr.addrlen as its value-result
 * length and the field is never reset, so after a datagram from a short
 * path every later, longer source address is cut to that length (and after
 * a datagram from an unbound socket the length is 0). Replies then fail or
 * go to the previous sender. nw_clt hides this because all of its
 * auto-generated client paths have the same length.
 */
# include "common.h"

static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_ses_send(ses, data, size);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    fprintf(stderr, "server error: %s\n", msg);
}

static int bound_client(const char *name, struct sockaddr_un *un)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    memset(un, 0, sizeof(*un));
    un->sun_family = AF_UNIX;
    snprintf(un->sun_path, sizeof(un->sun_path), "%s/%s", ut_tmpdir(), name);
    CHECK_EQ(bind(fd, (struct sockaddr *)un, sizeof(*un)), 0);
    return fd;
}

static bool round_trip(int fd, nw_addr_t *svr_addr)
{
    char buf[16];
    CHECK_EQ(sendto(fd, "ping\n", 5, 0, NW_SOCKADDR(svr_addr), svr_addr->addrlen), 5);
    ut_run_for(0.1);
    return recv(fd, buf, sizeof(buf), MSG_DONTWAIT) == 5;
}

int main(void)
{
    char cfg[256];
    ut_unix_cfg(cfg, sizeof(cfg), "dgram", "svr.sock");
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = svr_on_recv_pkg;
    st.on_error_msg = on_error_msg;
    ut_svr(cfg, &st, 1024, NULL);

    nw_addr_t svr_addr;
    int type;
    CHECK_EQ(nw_sock_cfg_parse(cfg, &svr_addr, &type), 0);

    struct sockaddr_un un;
    int short_fd = bound_client("a", &un);
    int long_fd = bound_client("a-much-longer-client-name", &un);

    CHECK(round_trip(short_fd, &svr_addr));
    CHECK(round_trip(long_fd, &svr_addr));      /* reply is lost: address truncated */
    return 0;
}
