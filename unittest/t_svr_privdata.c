/*
 * ses->privdata without on_privdata_alloc: the application puts its own
 * pointer there in on_open, uses it in the callbacks and takes care of it in
 * on_close. Closing such a connection used to call on_privdata_free, which
 * was not there (regression). With on_privdata_free alone, that callback
 * gets what the application put there.
 */
# include <sys/socket.h>
# include "common.h"

struct state {
    int messages;
};

static int opens, closes, frees, messages_at_close;
static bool free_in_on_close;

static void on_open(nw_ses *ses)
{
    CHECK(ses->privdata == NULL);
    struct state *state = calloc(1, sizeof(struct state));
    CHECK(state != NULL);
    ses->privdata = state;
    opens++;
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct state *state = ses->privdata;
    state->messages++;
}

static void on_close(nw_ses *ses)
{
    struct state *state = ses->privdata;
    messages_at_close = state->messages;
    if (free_in_on_close)
        free(state);
    closes++;
}

static void on_privdata_free(void *svr, void *privdata)
{
    free(privdata);
    frees++;
}

static nw_svr *server(bool with_free)
{
    static nw_svr_bind bind;
    memset(&bind, 0, sizeof(bind));
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    cfg.max_pkg_size = 1024;
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_open = on_open;
    type.on_close = on_close;
    if (with_free)
        type.on_privdata_free = on_privdata_free;
    free_in_on_close = !with_free;
    nw_svr *svr = nw_svr_create(&cfg, &type, NULL);
    CHECK(svr != NULL);
    return svr;
}

/* a connection of the server, and our end of it */
static int connection(nw_svr *svr)
{
    int sv[2];
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    nw_addr peer;
    memset(&peer, 0, sizeof(peer));
    peer.family = AF_UNIX;
    CHECK_EQ(nw_svr_add_accepted_fd(svr, 0, sv[0], &peer), 0);
    return sv[1];
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    for (int with_free = 0; with_free <= 1; ++with_free) {
        opens = closes = frees = 0;
        nw_svr *svr = server(with_free);

        /* closed by the peer */
        int fd = connection(svr);
        CHECK_EQ(opens, 1);
        CHECK_EQ(send(fd, "one\ntwo\n", 8, 0), 8);
        ut_run_for(0.02);
        close(fd);
        ut_run_for(0.02);
        CHECK_EQ(closes, 1);
        CHECK_EQ(messages_at_close, 2);
        CHECK_EQ(frees, with_free ? 1 : 0);

        /* closed by nw_svr_release */
        fd = connection(svr);
        CHECK_EQ(send(fd, "one\n", 4, 0), 4);
        ut_run_for(0.02);
        nw_svr_release(svr);
        CHECK_EQ(opens, 2);
        CHECK_EQ(closes, 2);
        CHECK_EQ(messages_at_close, 1);
        CHECK_EQ(frees, with_free ? 2 : 0);
        close(fd);
    }

    nw_loop_release();
    return 0;
}
