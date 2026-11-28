/*
 * wrapper/ut_rpc_clt with several servers. The list is handed to the
 * nw_clt underneath, which does the rest (see t_clt_list): a dead server
 * is passed over without a wait, on_connect(false) comes when all of them
 * have failed, and a server may be given by name.
 *
 * What the rpc_clt adds: on_close once for every connection that was
 * established and not for an attempt that failed, and a server that stops
 * answering heartbeats is left for the next one.
 *
 * Regression: rpc_clt used to go through the list itself, read its index
 * uninitialised when the very first connect failed, tried the first
 * address twice at the start and the last one again after every wait.
 */
# include <sys/socket.h>
# include <netinet/in.h>
# include "common.h"
# include "ut_rpc_svr.h"
# include "ut_rpc_clt.h"
# include "ut_crc32.h"

static int connect_ok;
static int connect_fail;
static int closes;
static char connected_to[NW_SOCK_HUMAN_ADDR_SIZE];

static void on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
}

static void clt_on_connect(nw_ses *ses, bool result)
{
    if (result) {
        connect_ok++;
        nw_sock_human_addr_s(&ses->peer_addr, connected_to);
    } else {
        connect_fail++;
    }
}

static void clt_on_close(nw_ses *ses)
{
    closes++;
}

# define WAIT(cond) do { \
    for (int _i = 0; _i < 1000 && !(cond); ++_i) \
        ut_run_for(0.01); \
    CHECK(cond); \
} while (0)

static rpc_svr *server(char *cfg_str, size_t size)
{
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
    snprintf(cfg_str, size, "tcp@127.0.0.1:%d", ut_svr_port(svr->raw_svr, 0));
    return svr;
}

static rpc_clt *client(const char **servers, int count, double reconnect)
{
    nw_addr addrs[8];
    rpc_clt_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (int i = 0; i < count; ++i)
        CHECK_EQ(nw_sock_addr_parse(servers[i], &addrs[i], &cfg.sock_type), 0);
    cfg.name = "failover";
    cfg.addr_count = count;
    cfg.addrs = addrs;
    cfg.reconnect_timeout = reconnect;
    rpc_clt_type type;
    memset(&type, 0, sizeof(type));
    type.on_recv_pkg = on_recv_pkg;
    type.on_connect = clt_on_connect;
    type.on_close = clt_on_close;
    rpc_clt *clt = rpc_clt_create(&cfg, &type);
    CHECK(clt != NULL);
    CHECK_EQ(rpc_clt_start(clt), 0);
    return clt;
}

static void reset(void)
{
    connect_ok = connect_fail = closes = 0;
    connected_to[0] = '\0';
}

/* every server dead: one failed attempt for the list, a wait, the next */
static void test_all_dead(void)
{
    char cfgs[3][64];
    const char *list[3];
    for (int i = 0; i < 3; ++i) {
        snprintf(cfgs[i], sizeof(cfgs[i]), "tcp@127.0.0.1:%d", ut_free_port(SOCK_STREAM));
        list[i] = cfgs[i];
    }
    reset();
    rpc_clt *clt = client(list, 3, 0.4);
    ut_run_for(0.2);
    CHECK_EQ(connect_fail, 1);
    ut_run_for(0.4);
    CHECK_EQ(connect_fail, 2);
    /* nothing was ever connected */
    CHECK_EQ(closes, 0);
    CHECK_EQ(rpc_clt_stop(clt), 0);
    CHECK_EQ(closes, 0);
    CHECK_EQ(rpc_clt_start(clt), 0);
    ut_run_for(0.2);
    CHECK_EQ(connect_fail, 3);
    rpc_clt_release(clt);
}

/* a server that takes the connection and never says a word: after the
 * heartbeat timeout the client goes on to the next one, and stays there */
static void test_heartbeat(void)
{
    int mute = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(mute >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(in);
    CHECK_EQ(bind(mute, (struct sockaddr *)&in, len), 0);
    CHECK_EQ(listen(mute, 16), 0);
    CHECK_EQ(getsockname(mute, (struct sockaddr *)&in, &len), 0);
    char mute_cfg[64], alive[64];
    snprintf(mute_cfg, sizeof(mute_cfg), "tcp@127.0.0.1:%d", ntohs(in.sin_port));
    rpc_svr *svr = server(alive, sizeof(alive));

    reset();
    const char *list[] = { mute_cfg, alive };
    rpc_clt *clt = client(list, 2, 30.0);
    WAIT(connect_ok == 1);
    CHECK(strcmp(connected_to, mute_cfg + 4) == 0);
    double begin = ev_time();
    WAIT(connect_ok == 2);
    fprintf(stderr, "left the mute server after %.1fs\n", ev_time() - begin);
    CHECK(ev_time() - begin >= RPC_HEARTBEAT_TIMEOUT - 0.5);
    CHECK(strcmp(connected_to, alive + 4) == 0);
    CHECK_EQ(closes, 1);
    CHECK_EQ(connect_fail, 0);
    /* this one answers */
    ut_run_for(RPC_HEARTBEAT_TIMEOUT + 1.5);
    CHECK_EQ(connect_ok, 2);
    CHECK(rpc_clt_connected(clt));
    rpc_clt_release(clt);
    ut_run_for(0.05);
    rpc_svr_release(svr);
    close(mute);
}

int main(void)
{
    /* the checksum is the standard CRC-32 (check value of "123456789") */
    CHECK_EQ(generate_crc32("123456789", 9), 0xCBF43926u);

    char dead[64], alive[64];
    rpc_svr *svr = server(alive, sizeof(alive));
    snprintf(dead, sizeof(dead), "tcp@127.0.0.1:%d", ut_free_port(SOCK_STREAM));

    /* the first is dead: the second is tried without waiting, and nothing
     * has failed as far as the application is concerned */
    const char *list[] = { dead, alive };
    rpc_clt *clt = client(list, 2, 30.0);
    double begin = ev_time();
    WAIT(connect_ok == 1);
    CHECK(ev_time() - begin < 3.0);
    CHECK_EQ(connect_fail, 0);
    CHECK(rpc_clt_connected(clt));
    CHECK(strcmp(connected_to, alive + 4) == 0);
    ut_run_for(0.05);
    CHECK_EQ(svr->raw_svr->ses_count, 1);
    CHECK_EQ(closes, 0);

    /* the server goes away: the connection is over, once, and then none
     * of the two can be reached */
    rpc_svr_release(svr);
    WAIT(connect_fail == 1);
    CHECK_EQ(closes, 1);
    CHECK(!rpc_clt_connected(clt));

    /* stopped while not connected: nothing to tell */
    CHECK_EQ(rpc_clt_stop(clt), 0);
    CHECK_EQ(closes, 1);
    rpc_clt_release(clt);

    /* a server given by name */
    svr = server(alive, sizeof(alive));
    char by_name[64];
    snprintf(by_name, sizeof(by_name), "tcp@localhost:%s", strrchr(alive, ':') + 1);
    reset();
    const char *named[] = { by_name };
    clt = client(named, 1, 30.0);
    WAIT(connect_ok == 1);
    CHECK(strcmp(connected_to, alive + 4) == 0);
    CHECK_EQ(connect_fail, 0);
    rpc_clt_release(clt);
    ut_run_for(0.05);
    rpc_svr_release(svr);

    test_all_dead();
    test_heartbeat();
    nw_loop_release();
    return 0;
}
