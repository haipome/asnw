/*
 * KNOWN BUG: nw_clt_release() does not stop clt->timer.
 * The connect-timeout timer armed by nw_clt_start stays active for
 * reconnect_timeout seconds even after a successful connect, so releasing
 * a client inside that window leaves libev pointing at freed memory and
 * the timer callback later runs on it.
 */
# include "common.h"

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    nw_loop_break();
}

int main(void)
{
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    nw_svr *svr = ut_svr("tcp@127.0.0.1:0", &st, 1024, NULL);

    char cfg[64];
    snprintf(cfg, sizeof(cfg), "tcp@127.0.0.1:%d", ut_svr_port(svr, 0));
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = on_connect;
    nw_clt *clt = ut_clt(cfg, &ct, 1024, 0.2, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(1.0);
    CHECK(nw_clt_connected(clt));

    nw_clt_release(clt);
    ut_run_for(0.5);        /* the stale timer fires in here */
    return 0;
}
