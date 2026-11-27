/*
 * wrapper/ut_rpc_clt: with the first address dead, the client moves on to
 * the next one. Also a regression test: rpc_clt.curr_index used to be read
 * uninitialised when the very first connect failed (visible under valgrind).
 *
 * The order: every address of the list once, a wait, the list again. It
 * used to try the first address twice at the start, and the last one again
 * after every wait (regression).
 *
 * on_close: once for every connection that was established, not for an
 * attempt that failed.
 */
# include "common.h"
# include "ut_rpc_svr.h"
# include "ut_rpc_clt.h"
# include "ut_crc32.h"

static int connect_ok;
static int connect_fail;
static int closes;
static int tried[16], tries;
static int ports[3];

static void on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
}

static void clt_on_connect(nw_ses *ses, bool result)
{
    if (result) {
        connect_ok++;
        nw_loop_break();
    } else {
        connect_fail++;
    }
}

static void clt_on_close(nw_ses *ses)
{
    closes++;
}

/* which address of the list was it? */
static void order_on_connect(nw_ses *ses, bool result)
{
    CHECK(!result);
    int port = ntohs(ses->peer_addr.in.sin_port);
    for (int i = 0; i < 3; ++i) {
        if (port == ports[i] && tries < 16)
            tried[tries++] = i + 1;
    }
}

static void test_order(void)
{
    nw_addr addrs[3];
    rpc_clt_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (int i = 0; i < 3; ++i) {
        char str[64];
        ports[i] = ut_free_port(SOCK_STREAM);
        snprintf(str, sizeof(str), "tcp@127.0.0.1:%d", ports[i]);
        CHECK_EQ(nw_sock_addr_parse(str, &addrs[i], &cfg.sock_type), 0);
    }
    cfg.name = "order";
    cfg.addr_count = 3;
    cfg.addrs = addrs;
    cfg.reconnect_timeout = 0.4;
    rpc_clt_type type;
    memset(&type, 0, sizeof(type));
    type.on_recv_pkg = on_recv_pkg;
    type.on_connect = order_on_connect;
    type.on_close = clt_on_close;
    rpc_clt *clt = rpc_clt_create(&cfg, &type);
    CHECK(clt != NULL);
    CHECK_EQ(rpc_clt_start(clt), 0);

    /* the list at once, and nothing more before the wait is over */
    ut_run_for(0.2);
    CHECK_EQ(tries, 3);
    /* the wait, and the list again */
    ut_run_for(0.4);
    CHECK_EQ(tries, 6);
    for (int i = 0; i < 6; ++i)
        CHECK_EQ(tried[i], i % 3 + 1);
    /* nothing was ever connected */
    CHECK_EQ(closes, 0);

    /* stopped and started: from the first address again */
    CHECK_EQ(rpc_clt_stop(clt), 0);
    tries = 0;
    CHECK_EQ(rpc_clt_start(clt), 0);
    ut_run_for(0.2);
    CHECK_EQ(tries, 3);
    for (int i = 0; i < 3; ++i)
        CHECK_EQ(tried[i], i + 1);
    CHECK_EQ(closes, 0);
    rpc_clt_release(clt);
}

int main(void)
{
    /* the checksum is the standard CRC-32 (check value of "123456789") */
    CHECK_EQ(generate_crc32("123456789", 9), 0xCBF43926u);

    nw_svr_bind bind;
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    rpc_svr_type st;
    memset(&st, 0, sizeof(st));
    st.on_recv_pkg = on_recv_pkg;
    rpc_svr *svr = rpc_svr_create(&cfg, &st);
    CHECK(svr != NULL);
    CHECK_EQ(rpc_svr_start(svr), 0);

    char dead[64], alive[64];
    snprintf(dead, sizeof(dead), "tcp@127.0.0.1:%d", ut_free_port(SOCK_STREAM));
    snprintf(alive, sizeof(alive), "tcp@127.0.0.1:%d", ut_svr_port(svr->raw_svr, 0));
    nw_addr addrs[2];
    rpc_clt_cfg ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    CHECK_EQ(nw_sock_addr_parse(dead, &addrs[0], &ccfg.sock_type), 0);
    CHECK_EQ(nw_sock_addr_parse(alive, &addrs[1], &ccfg.sock_type), 0);
    ccfg.name = "failover";
    ccfg.addr_count = 2;
    ccfg.addrs = addrs;
    ccfg.reconnect_timeout = 30.0;      /* the second address must be tried without waiting */

    rpc_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = clt_on_connect;
    ct.on_close = clt_on_close;
    rpc_clt *clt = rpc_clt_create(&ccfg, &ct);
    CHECK(clt != NULL);
    CHECK_EQ(rpc_clt_start(clt), 0);

    ut_run_for(3.0);
    CHECK_EQ(connect_ok, 1);
    /* the dead address once, not twice */
    CHECK_EQ(connect_fail, 1);
    CHECK(rpc_clt_connected(clt));
    CHECK(strcmp(nw_sock_human_addr(&clt->raw_clt->ses.peer_addr), alive + 4) == 0);
    ut_run_for(0.05);
    CHECK_EQ(svr->raw_svr->ses_count, 1);
    /* the attempt that failed was not a connection that closed */
    CHECK_EQ(closes, 0);

    /* the server goes away: the connection is over, once, and the client
     * is back at the first address */
    rpc_svr_release(svr);
    ut_run_for(0.1);
    CHECK_EQ(closes, 1);
    CHECK(!rpc_clt_connected(clt));
    CHECK_EQ(connect_fail, 3);

    /* stopped while not connected: nothing to tell */
    CHECK_EQ(rpc_clt_stop(clt), 0);
    CHECK_EQ(closes, 1);
    rpc_clt_release(clt);

    closes = 0;
    test_order();
    nw_loop_release();
    return 0;
}
