/*
 * KNOWN BUG: a datagram nw_clt sends to whoever wrote to it last.
 * on_can_read() does recvfrom(..., &ses->peer_addr, ...) on the client
 * session too, overwriting the configured server address with the source
 * of the last datagram received. One stray (or spoofed) packet from any
 * other address silently redirects everything the client sends afterwards.
 */
# include "common.h"

static int svr_recv;

static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    svr_recv++;
}

static void clt_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

int main(void)
{
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = svr_on_recv_pkg;
    nw_svr *svr = ut_svr("udp@127.0.0.1:0", &st, 1024, NULL);

    char cfg[64];
    snprintf(cfg, sizeof(cfg), "udp@127.0.0.1:%d", ut_svr_port(svr, 0));
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = clt_on_recv_pkg;
    nw_clt *clt = ut_clt(cfg, &ct, 1024, 1.0, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);

    CHECK_EQ(nw_ses_send(&clt->ses, "one\n", 4), 0);
    ut_run_for(0.1);
    CHECK_EQ(svr_recv, 1);

    /* a third party sends one datagram to the client's port */
    nw_addr_t clt_addr;
    CHECK_EQ(nw_sock_host_addr(clt->ses.sockfd, &clt_addr), 0);
    clt_addr.in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int other = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(other >= 0);
    CHECK_EQ(sendto(other, "hi\n", 3, 0, NW_SOCKADDR(&clt_addr), sizeof(clt_addr.in)), 3);
    ut_run_for(0.1);

    /* the client still believes it talks to the server */
    CHECK_EQ(nw_ses_send(&clt->ses, "two\n", 4), 0);
    ut_run_for(0.1);
    fprintf(stderr, "client now sends to %s, server is %s\n", nw_sock_human_addr(&clt->ses.peer_addr), cfg + 4);
    CHECK_EQ(svr_recv, 2);
    return 0;
}
