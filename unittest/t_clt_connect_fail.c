/*
 * Regression (fixed): a connect() that fails on the spot (unix socket path
 * missing, backlog full ...) used to run the failure path twice: on_close
 * was called twice per attempt, a wrapper rotating addresses in on_close
 * (wrapper/ut_rpc_clt.c) skipped every other address, and an on_close
 * returning > 0 ("reconnect now") recursed until the stack overflowed.
 * Now: one on_connect(false) and one on_close per attempt, and the
 * immediate retry goes through the event loop.
 */
# include "common.h"

static int close_count;
static int connect_fail;
static int connect_ok;
static int reconnect_now;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    if (result) {
        connect_ok++;
        nw_loop_break();
    } else {
        CHECK_EQ(errno, ENOENT);
        connect_fail++;
    }
}

static int on_close(nw_ses *ses)
{
    close_count++;
    return reconnect_now;
}

int main(void)
{
    char cfg[256];
    ut_unix_cfg(cfg, sizeof(cfg), "stream", "nobody-listens.sock");
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = on_connect;
    ct.on_close = on_close;

    /* retry after reconnect_timeout */
    nw_clt *clt = ut_clt(cfg, &ct, 1024, 0.05, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    CHECK_EQ(connect_fail, 1);
    CHECK_EQ(close_count, 1);
    CHECK(!nw_clt_connected(clt));
    ut_run_for(0.22);
    CHECK(connect_fail >= 3 && connect_fail <= 6);
    CHECK_EQ(close_count, connect_fail);
    nw_clt_release(clt);

    /* "reconnect now": retried from the loop, as fast as it spins, no recursion */
    reconnect_now = 1;
    connect_fail = close_count = 0;
    clt = ut_clt(cfg, &ct, 1024, 60.0, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    CHECK_EQ(connect_fail, 1);
    CHECK_EQ(close_count, 1);
    ut_run_for(0.1);
    fprintf(stderr, "%d immediate retries in 0.1s\n", connect_fail);
    CHECK(connect_fail >= 20);
    CHECK_EQ(close_count, connect_fail);

    /* and it gets through as soon as somebody listens */
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    nw_svr *svr = ut_svr(cfg, &st, 1024, NULL);
    ut_run_for(1.0);
    CHECK_EQ(connect_ok, 1);
    CHECK(nw_clt_connected(clt));

    nw_clt_release(clt);
    ut_run_for(0.05);
    nw_svr_release(svr);
    return 0;
}
