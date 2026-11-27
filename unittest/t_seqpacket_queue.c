/*
 * Regression (fixed): queued seqpacket messages are silently dropped.
 * In on_can_write() (nw_ses.c) the result of write_packet() is an int
 * that is compared with a size_t: `if (nwrite < size)`. When sendmsg()
 * returns -1 (EAGAIN while flushing the pending list) the -1 is converted
 * to SIZE_MAX, the error branch is skipped and the packet is shifted off
 * the list as if it had been sent. Real errors are swallowed the same way.
 */
# include "common.h"

# define PKG_SIZE (100 * 1024)
# define TOTAL    20

static int svr_recv;
static int errors;

static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    CHECK_EQ(size, PKG_SIZE);
    svr_recv++;
}

static void clt_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static int whole_decode(nw_ses *ses, void *data, size_t max)
{
    return max;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    errors++;
}

static void clt_on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    char *pkg = malloc(PKG_SIZE);
    memset(pkg, 'x', PKG_SIZE);
    /* far more than the socket buffer holds: most of these get queued */
    for (int i = 0; i < TOTAL; ++i)
        CHECK_EQ(nw_ses_send(ses, pkg, PKG_SIZE), 0);
    free(pkg);
}

int main(void)
{
    char cfg[256];
    ut_unix_cfg(cfg, sizeof(cfg), "seqpacket", "drop.sock");

    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = whole_decode;
    st.on_recv_pkg = svr_on_recv_pkg;
    st.on_error_msg = on_error_msg;
    ut_svr(cfg, &st, PKG_SIZE, NULL);

    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = whole_decode;
    ct.on_recv_pkg = clt_on_recv_pkg;
    ct.on_connect = clt_on_connect;
    ct.on_error_msg = on_error_msg;
    nw_clt *clt = ut_clt(cfg, &ct, PKG_SIZE, 1.0, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);

    ut_run_for(1.0);
    fprintf(stderr, "sent %d packets, every send returned 0, no error reported (%d), server received %d\n",
            TOTAL, errors, svr_recv);
    CHECK_EQ(svr_recv, TOTAL);
    return 0;
}
