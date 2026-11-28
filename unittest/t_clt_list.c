/*
 * nw_clt with several servers: they have their turn one after the other,
 * round and round. A server that fails is reported by on_error_msg and the
 * next one is tried at once; a connection that is lost sends the client on
 * to the next one as well, not back to the first; only when all of them
 * have failed one after the other is the attempt reported as failed, and
 * does the client wait. nw_clt_start goes on where the client stands. A
 * dgram client stays with the first server.
 */
# include "common.h"

# define SERVERS 3

static nw_svr *svrs[SERVERS];
static int ports[SERVERS];
static int received[SERVERS];

static nw_clt *clt;
static int connect_ok, connect_fail, close_count, error_count;
/* the server the client is connected to, and the ones that failed, in order */
static int connected_to;
static int failed[64], failed_count;

static int server_of(int port)
{
    for (int i = 0; i < SERVERS; ++i) {
        if (ports[i] == port)
            return i;
    }
    return -1;
}

static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_svr *svr = ses->svr;
    received[(int)(intptr_t)svr->privdata]++;
    nw_ses_send(ses, data, size);
}

static void server_start(int i, const char *proto)
{
    char cfg[64];
    snprintf(cfg, sizeof(cfg), "%s@127.0.0.1:%d", proto, ports[i]);
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = svr_on_recv_pkg;
    svrs[i] = ut_svr(cfg, &st, 1024, (void *)(intptr_t)i);
    if (ports[i] == 0)
        ports[i] = ut_svr_port(svrs[i], 0);
}

static void server_stop(int i)
{
    nw_svr_release(svrs[i]);
    svrs[i] = NULL;
}

/* the server lets go of its connections, and stays */
static void server_drop(int i)
{
    nw_svr_iterator iter;
    nw_ses *ses;
    nw_svr_iter_init(&iter, svrs[i]);
    while ((ses = nw_svr_next(&iter)) != NULL)
        nw_svr_close(svrs[i], ses);
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    if (result) {
        connect_ok++;
        connected_to = server_of(ntohs(ses->peer_addr.in.sin_port));
    } else {
        connect_fail++;
        CHECK_EQ(errno, ECONNREFUSED);
    }
}

static void on_close(nw_ses *ses)
{
    close_count++;
}

/* "connect 127.0.0.1:<port>: Connection refused" */
static void on_error_msg(nw_ses *ses, const char *msg)
{
    error_count++;
    int port = 0;
    if (sscanf(msg, "connect 127.0.0.1:%d:", &port) == 1 && failed_count < 64)
        failed[failed_count++] = server_of(port);
}

# define WAIT(cond) do { \
    for (int _i = 0; _i < 1000 && !(cond); ++_i) \
        ut_run_for(0.01); \
    CHECK(cond); \
} while (0)

static void client_create(const char *proto, double reconnect)
{
    char cfgs[SERVERS][64];
    const char *list[SERVERS];
    for (int i = 0; i < SERVERS; ++i) {
        snprintf(cfgs[i], sizeof(cfgs[i]), "%s@127.0.0.1:%d", proto, ports[i]);
        list[i] = cfgs[i];
    }
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = on_connect;
    ct.on_close = on_close;
    ct.on_error_msg = on_error_msg;
    clt = ut_clt_list(list, SERVERS, &ct, 1024, reconnect, NULL);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < SERVERS; ++i)
        server_start(i, "tcp");

    /* all three are there: the first, and after each lost connection the
     * next, round to the first again. no wait, no failure */
    client_create("tcp", 0.4);
    CHECK_EQ(nw_clt_start(clt), 0);
    double begin = ev_time();
    for (int n = 0; n < 5; ++n) {
        WAIT(connect_ok == n + 1);
        CHECK_EQ(connected_to, n % SERVERS);
        CHECK_EQ(close_count, n);
        server_drop(connected_to);
    }
    WAIT(connect_ok == 6);
    CHECK_EQ(connected_to, 2);
    CHECK(ev_time() - begin < 0.3);
    CHECK_EQ(error_count, 0);
    CHECK_EQ(connect_fail, 0);

    /* nw_clt_stop and nw_clt_start: on_close, and on with the next one,
     * not back to the first */
    CHECK_EQ(nw_clt_stop(clt), 0);
    CHECK_EQ(close_count, 6);
    CHECK(!nw_clt_connected(clt));
    CHECK_EQ(nw_clt_start(clt), 0);
    WAIT(connect_ok == 7);
    CHECK_EQ(connected_to, 0);
    CHECK_EQ(nw_clt_stop(clt), 0);
    CHECK_EQ(close_count, 7);
    CHECK_EQ(nw_clt_start(clt), 0);
    WAIT(connect_ok == 8);
    CHECK_EQ(connected_to, 1);

    /* the third is gone: lost on the second, the client says that the
     * third can't be reached and is with the first at once */
    server_stop(2);
    begin = ev_time();
    server_drop(1);
    WAIT(connect_ok == 9);
    CHECK(ev_time() - begin < 0.3);
    CHECK_EQ(connected_to, 0);
    CHECK_EQ(close_count, 8);
    CHECK_EQ(error_count, 1);
    CHECK_EQ(failed_count, 1);
    CHECK_EQ(failed[0], 2);
    CHECK_EQ(connect_fail, 0);

    /* all gone. the three have their turn at once, beginning with the one
     * after the server that was lost; then the attempt has failed, and
     * the client waits */
    server_stop(1);
    failed_count = 0;
    begin = ev_time();
    server_stop(0);
    WAIT(connect_fail == 1);
    CHECK(ev_time() - begin < 0.3);
    CHECK_EQ(failed_count, 3);
    CHECK_EQ(failed[0], 1);
    CHECK_EQ(failed[1], 2);
    CHECK_EQ(failed[2], 0);
    /* on_close for the connection that was lost, and for the attempt */
    CHECK_EQ(close_count, 10);
    CHECK(nw_timer_active(&clt->timer));
    ut_run_for(0.2);
    CHECK_EQ(connect_fail, 1);
    CHECK_EQ(failed_count, 3);
    /* and goes through them again, on from where it stopped */
    WAIT(connect_fail == 2);
    CHECK(ev_time() - begin >= 0.35);
    CHECK_EQ(failed_count, 6);
    for (int i = 0; i < 6; ++i)
        CHECK_EQ(failed[i], (i + 1) % SERVERS);
    CHECK_EQ(close_count, 11);

    /* the first comes back: found in the next round, after the other two
     * have failed once more */
    failed_count = 0;
    server_start(0, "tcp");
    WAIT(connect_ok == 10);
    CHECK_EQ(connected_to, 0);
    CHECK_EQ(failed_count, 2);
    CHECK_EQ(failed[0], 1);
    CHECK_EQ(failed[1], 2);
    CHECK_EQ(connect_fail, 2);
    nw_clt_release(clt);
    server_stop(0);
    ut_run_for(0.05);

    /* dgram: nothing tells that a server has failed, the client stays
     * with the first */
    ports[0] = ports[1] = ports[2] = 0;
    for (int i = 0; i < SERVERS; ++i)
        server_start(i, "udp");
    connect_ok = connect_fail = close_count = error_count = 0;
    client_create("udp", 0.1);
    CHECK_EQ(nw_clt_start(clt), 0);
    CHECK(nw_clt_connected(clt));
    for (int round = 0; round < 3; ++round) {
        CHECK_EQ(nw_ses_send(&clt->ses, "datagram\n", 9), 0);
        WAIT(received[0] == round + 1);
        CHECK_EQ(nw_clt_stop(clt), 0);
        CHECK_EQ(nw_clt_start(clt), 0);
    }
    CHECK_EQ(received[1], 0);
    CHECK_EQ(received[2], 0);
    CHECK_EQ(connect_ok, 0);
    CHECK_EQ(error_count, 0);
    nw_clt_release(clt);

    for (int i = 0; i < SERVERS; ++i)
        server_stop(i);
    nw_loop_release();
    return 0;
}
