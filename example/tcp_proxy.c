/*
 * Description: example: a tcp proxy. every connection it accepts gets a
 *              connection of its own to the upstream server, and what comes
 *              in on one of the two goes out on the other.
 *              tcp_proxy 'tcp@0.0.0.0:8000' 'tcp@db.internal:5432'
 *
 * what it is here to show:
 *
 *   - flow control. a proxy must not read faster from one side than the
 *     other side takes, or it piles up in memory what the slow side has not
 *     read yet. after every send it looks at nw_ses_pending of the
 *     receiving side: over HIGH_WATER, it stops reading from the sending
 *     side (nw_ses_pause_read) and reads again when the receiving side has
 *     taken everything (on_drain, nw_ses_resume_read). the memory a pair of
 *     connections can hold is then HIGH_WATER + BUF_SIZE in each direction,
 *     whatever the two ends do.
 *
 *   - a nw_svr connection and a nw_clt that belong together. the client
 *     connection is known by its id, never by a pointer kept across
 *     callbacks, and ses->privdata of both points at the struct link that
 *     joins them.
 *
 *   - a nw_clt that must not reconnect: nw_clt_stop in its on_close. and
 *     one that cannot be released on the spot, because its end is always
 *     learned inside one of its own callbacks: it is released from a timer.
 *
 * what it leaves out: timeouts. a client or a server that stops reading
 * keeps its pair alive for as long as its own connection lasts. and a
 * client that closes its sending side to say "that was all" and then waits
 * for the answer does not get it: see the half-close paragraph of nw_svr.h.
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>

# include "nw_svr.h"
# include "nw_clt.h"
# include "nw_buf.h"
# include "nw_loop.h"
# include "nw_timer.h"

/* how much is read in one go, and the size of the bufs things wait in */
# define BUF_SIZE   (64 * 1024)
/* more than this waiting for one side: stop reading from the other */
# define HIGH_WATER (256 * 1024)
/* a connect to the upstream server that takes longer has failed */
# define CONNECT_TIMEOUT 5.0

struct link {
    /* the connection of the client, a connection of svr */
    uint64_t     id;
    /* our connection to the upstream server */
    nw_clt      *up;
    bool         down_gone;
    bool         up_gone;
    /* among the dead, until the reaper comes */
    struct link *next;
};

static nw_svr *svr;
static nw_clt_cfg up_cfg;
static nw_clt_type up_type;
static struct link *dead;
static nw_timer reaper;

/* release the links that were buried. not done in bury itself: there we
 * may be inside a callback of the nw_clt, which must not be released from
 * one, so a timer does it on the next turn of the loop */
static void on_reap(nw_timer *timer, void *privdata)
{
    while (dead) {
        struct link *link = dead;
        dead = link->next;
        if (link->up)
            nw_clt_release(link->up);
        free(link);
    }
}

/* called only when both ends are gone, by the one that ends last:
 *
 *   the server first, the client later:  on_close of the client, which
 *                                        finds up_gone set
 *   the client first, the server later:  up_on_close, which finds
 *                                        down_gone set
 */
static void bury(struct link *link)
{
    link->next = dead;
    dead = link;
    if (!nw_timer_active(&reaper)) {
        nw_timer_set(&reaper, 0, false, on_reap, NULL);
        nw_timer_start(&reaper);
    }
}

/* a stream without messages: whatever has arrived is passed on */
static int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    return max;
}

/*
 * the side of the client: connections of svr
 */

static void on_open(nw_ses *ses)
{
    struct link *link = calloc(1, sizeof(struct link));
    if (link == NULL) {
        nw_svr_close(svr, ses);
        return;
    }
    link->id = ses->id;
    ses->privdata = link;
    /* nothing can be passed on before the upstream server is connected */
    nw_ses_pause_read(ses);

    link->up = nw_clt_create(&up_cfg, &up_type, link);
    if (link->up == NULL || nw_clt_start(link->up) < 0) {
        link->up_gone = true;
        nw_svr_close(svr, ses);
    }
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct link *link = ses->privdata;
    nw_ses *up = &link->up->ses;
    /* a send that fails has closed the upstream connection: up_on_close
     * was called and has seen to this one */
    if (nw_ses_send(up, data, size) < 0)
        return;
    if (nw_ses_pending(up) > HIGH_WATER)
        nw_ses_pause_read(ses);
}

/* the client has taken everything: the server may send more, or, if it
 * is gone, this was the last of it */
static void on_drain(nw_ses *ses)
{
    struct link *link = ses->privdata;
    if (link->up_gone)
        nw_svr_shutdown(svr, ses);
    else
        nw_ses_resume_read(&link->up->ses);
}

static void on_close(nw_ses *ses)
{
    struct link *link = ses->privdata;
    printf("%s: closed\n", nw_sock_human_addr(&ses->peer_addr));
    if (link == NULL)
        return;
    link->down_gone = true;
    if (link->up_gone) {
        bury(link);
        return;
    }
    /* what the client sent still has to reach the server: up_on_drain
     * closes the upstream connection when it has */
    if (nw_clt_connected(link->up) && nw_ses_pending(&link->up->ses) > 0)
        return;
    nw_clt_stop(link->up);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    printf("%s: %s\n", nw_sock_human_addr(&ses->peer_addr), msg);
}

/*
 * the side of the upstream server: one nw_clt for each client
 */

static void up_on_connect(nw_ses *ses, bool result)
{
    struct link *link = ses->privdata;
    /* failed: on_close is called next */
    if (!result)
        return;
    nw_ses *down = nw_svr_get(svr, link->id);
    if (down == NULL)
        return;
    char up_addr[NW_SOCK_HUMAN_ADDR_SIZE];
    nw_sock_human_addr_s(&ses->peer_addr, up_addr);
    printf("%s: connected to %s\n", nw_sock_human_addr(&down->peer_addr), up_addr);
    nw_ses_resume_read(down);
}

static void up_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct link *link = ses->privdata;
    nw_ses *down = nw_svr_get(svr, link->id);
    if (down == NULL)
        return;
    if (nw_ses_send(down, data, size) < 0)
        return;
    if (nw_ses_pending(down) > HIGH_WATER)
        nw_ses_pause_read(ses);
}

/* the server has taken everything: the client may send more, or, if it
 * is gone, this was the last of it */
static void up_on_drain(nw_ses *ses)
{
    struct link *link = ses->privdata;
    if (link->down_gone) {
        nw_clt_stop(link->up);
        return;
    }
    nw_ses *down = nw_svr_get(svr, link->id);
    if (down)
        nw_ses_resume_read(down);
}

static int up_on_close(nw_ses *ses)
{
    struct link *link = ses->privdata;
    if (link->up_gone)
        return 0;
    link->up_gone = true;
    /* one connection for one client: stay closed, don't connect again */
    nw_clt_stop(link->up);
    if (link->down_gone) {
        bury(link);
        return 0;
    }
    /* what is queued for the client goes out first, however long the
     * client takes: on_drain closes the connection then. nw_svr_shutdown
     * right away would give it 2 seconds */
    nw_ses *down = nw_svr_get(svr, link->id);
    if (down && nw_ses_pending(down) == 0)
        nw_svr_shutdown(svr, down);
    return 0;
}

static void up_on_error_msg(nw_ses *ses, const char *msg)
{
    printf("upstream: %s\n", msg);
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: %s listen upstream\n", argv[0]);
        printf("   eg: %s 'tcp@0.0.0.0:8000' 'tcp@db.internal:5432'\n", argv[0]);
        exit(0);
    }

    nw_svr_bind bind;
    memset(&bind, 0, sizeof(bind));
    if (nw_sock_addr_parse(argv[1], &bind.addr, &bind.sock_type) < 0 || bind.sock_type != SOCK_STREAM) {
        printf("listen: %s is not a stream address\n", argv[1]);
        exit(1);
    }
    memset(&up_cfg, 0, sizeof(up_cfg));
    if (nw_clt_cfg_parse(argv[2], &up_cfg) < 0 || up_cfg.sock_type != SOCK_STREAM) {
        printf("upstream: %s is not a stream address\n", argv[2]);
        exit(1);
    }

    /* the bufs of all the upstream connections come from one pool */
    up_cfg.buf_pool = nw_buf_pool_create(BUF_SIZE);
    if (up_cfg.buf_pool == NULL) {
        printf("nw_buf_pool_create fail\n");
        exit(1);
    }
    up_cfg.max_pkg_size = BUF_SIZE;
    up_cfg.reconnect_timeout = CONNECT_TIMEOUT;

    memset(&up_type, 0, sizeof(up_type));
    up_type.decode_pkg = decode_pkg;
    up_type.on_connect = up_on_connect;
    up_type.on_recv_pkg = up_on_recv_pkg;
    up_type.on_drain = up_on_drain;
    up_type.on_close = up_on_close;
    up_type.on_error_msg = up_on_error_msg;

    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    cfg.max_pkg_size = BUF_SIZE;

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = decode_pkg;
    type.on_open = on_open;
    type.on_recv_pkg = on_recv_pkg;
    type.on_drain = on_drain;
    type.on_close = on_close;
    type.on_error_msg = on_error_msg;

    svr = nw_svr_create(&cfg, &type, NULL);
    if (svr == NULL) {
        printf("nw_svr_create fail\n");
        exit(1);
    }
    if (nw_svr_start(svr) < 0) {
        printf("nw_svr_start fail: %m\n");
        exit(1);
    }

    printf("%s -> %s\n", argv[1], argv[2]);
    nw_loop_run();

    return 0;
}
