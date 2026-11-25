/*
 * KNOWN BUG: an immediately failing connect is handled twice.
 * When connect() fails synchronously (unix socket path missing, backlog
 * full ...), nw_ses_connect() already calls ses->on_connect(false), which
 * runs the whole failure path (user on_close, close, reconnect). It then
 * returns -1 and nw_clt_start() runs the same failure path again.
 * Effects: on_close is called twice per failed attempt; a wrapper that
 * rotates addresses in on_close (wrapper/ut_rpc_clt.c) skips every other
 * address; and if on_close returns > 0 ("reconnect now") the two paths
 * recurse into nw_clt_start without bound until the stack overflows
 * (run this test with an argument to see that).
 */
# include "common.h"

static int close_count;
static int connect_fail;
static int reconnect_now;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    CHECK(!result);
    connect_fail++;
}

static int on_close(nw_ses *ses)
{
    close_count++;
    return reconnect_now;
}

int main(int argc, char *argv[])
{
    reconnect_now = argc > 1;

    char cfg[256];
    ut_unix_cfg(cfg, sizeof(cfg), "stream", "nobody-listens.sock");
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = on_connect;
    ct.on_close = on_close;
    nw_clt *clt = ut_clt(cfg, &ct, 1024, 60.0, NULL);

    nw_clt_start(clt);
    CHECK_EQ(connect_fail, 1);
    CHECK_EQ(close_count, 1);
    return 0;
}
