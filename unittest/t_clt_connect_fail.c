/*
 * A connect() that fails on the spot (unix socket path missing, backlog
 * full ...), as opposed to one whose failure the loop reports later.
 *
 * Regression (fixed): it used to run the failure path twice, on_close was
 * called twice per attempt, and a retry without delay recursed until the
 * stack overflowed.
 *
 * Now nothing is called from inside nw_clt_start; the failure is dealt
 * with on the next turn of the loop like any other: on_error_msg for the
 * server, and, when it was the last one, on_connect(false) and on_close.
 * Going on to the next server takes no time and does not recurse, however
 * many servers fail that way.
 */
# include "common.h"

static int close_count;
static int connect_fail;
static int connect_ok;
static int error_count;
static char last_error[256];
static char connected_to[NW_SOCK_HUMAN_ADDR_SIZE];

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    if (result) {
        connect_ok++;
        nw_sock_human_addr_s(&ses->peer_addr, connected_to);
        nw_loop_break();
    } else {
        CHECK_EQ(errno, ENOENT);
        connect_fail++;
    }
}

static void on_close(nw_ses *ses)
{
    close_count++;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    error_count++;
    snprintf(last_error, sizeof(last_error), "%s", msg);
}

# define SERVERS 16

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
    ct.on_error_msg = on_error_msg;

    /* one server: the attempt fails with it, and is repeated after
     * reconnect_timeout */
    nw_clt *clt = ut_clt(cfg, &ct, 1024, 0.05, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    /* nothing was called from inside nw_clt_start */
    CHECK_EQ(error_count, 0);
    CHECK_EQ(connect_fail, 0);
    CHECK_EQ(close_count, 0);
    CHECK(!nw_clt_connected(clt));
    ut_run_for(0.02);
    CHECK_EQ(error_count, 1);
    CHECK_EQ(connect_fail, 1);
    CHECK_EQ(close_count, 1);
    CHECK(strncmp(last_error, "connect unix:", 13) == 0);
    CHECK(strstr(last_error, "nobody-listens.sock: No such file or directory") != NULL);
    ut_run_for(0.2);
    CHECK(connect_fail >= 3 && connect_fail <= 6);
    CHECK_EQ(close_count, connect_fail);
    CHECK_EQ(error_count, connect_fail);
    nw_clt_release(clt);

    /* many servers, all failing on the spot: they all have their turn at
     * once, and then the client waits */
    char cfgs[SERVERS][256];
    const char *list[SERVERS];
    for (int i = 0; i < SERVERS; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "nobody-%d.sock", i);
        ut_unix_cfg(cfgs[i], sizeof(cfgs[i]), "stream", name);
        list[i] = cfgs[i];
    }
    connect_fail = close_count = error_count = 0;
    clt = ut_clt_list(list, SERVERS, &ct, 1024, 60.0, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.2);
    CHECK_EQ(error_count, SERVERS);
    CHECK_EQ(connect_fail, 1);
    CHECK_EQ(close_count, 1);
    CHECK(strstr(last_error, "nobody-15.sock") != NULL);
    CHECK(nw_timer_active(&clt->timer));
    nw_clt_release(clt);

    /* the last of them is there: the client gets to it without a wait and
     * without a failed attempt */
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    nw_svr *svr = ut_svr(cfgs[SERVERS - 1], &st, 1024, NULL);
    connect_fail = close_count = error_count = 0;
    clt = ut_clt_list(list, SERVERS, &ct, 1024, 60.0, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(1.0);
    CHECK_EQ(connect_ok, 1);
    CHECK(nw_clt_connected(clt));
    CHECK(strstr(connected_to, "nobody-15.sock") != NULL);
    CHECK_EQ(error_count, SERVERS - 1);
    CHECK_EQ(connect_fail, 0);
    CHECK_EQ(close_count, 0);

    nw_clt_release(clt);
    ut_run_for(0.05);
    nw_svr_release(svr);
    return 0;
}
