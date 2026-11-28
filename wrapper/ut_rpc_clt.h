/*
 * Description: ut_rpc client, a nw_clt speaking rpc_pkg
 *     History: yang@haipo.me, 2016/03/30, create
 */

# ifndef _UT_RPC_CLT_H_
# define _UT_RPC_CLT_H_

# include "ut_rpc.h"
# include "nw_clt.h"
# include "nw_timer.h"

/*
 * rpc_clt keeps a connection to one of several equivalent servers, the
 * way a nw_clt does (see nw_clt.h): they have their turn one after the
 * other, each may be an address or the name of a host, and the client
 * waits reconnect_timeout only when all of them have failed. heartbeats
 * are sent automatically (see ut_rpc.h), and a server that stops
 * answering them is left for the next one.
 *
 * in the callbacks ses->privdata is the rpc_clt.
 */

typedef struct rpc_clt_cfg {
    /* must: a name for the peer, for your logs. copied */
    char *name;
    /* number of entries in addrs, at least 1 */
    uint32_t addr_count;
    /* the servers, addresses or names, as nw_sock_addr_parse gives them,
     * all of the same sock_type. copied */
    nw_addr *addrs;
    /* SOCK_STREAM, SOCK_DGRAM or SOCK_SEQPACKET */
    int sock_type;
    /* see nw_clt_cfg */
    uint32_t read_mem;
    uint32_t write_mem;
    double reconnect_timeout;
} rpc_clt_cfg;

typedef struct rpc_clt_type {
    /* must
     *
     * called for every packet received, except heartbeats. the header is
     * in host byte order; pkg, pkg->ext and pkg->body are only valid
     * during the call */
    void (*on_recv_pkg)(nw_ses *ses, rpc_pkg *pkg);
    /* optional
     *
     * called when an attempt to connect ends, see nw_clt_type.on_connect:
     * with true when connected, with false when none of the servers could
     * be reached */
    void (*on_connect)(nw_ses *ses, bool result);
    /* optional
     *
     * called when a connection that was established is over: lost, given
     * up after a heartbeat timeout, or closed by rpc_clt_stop (not by
     * rpc_clt_release, which calls nothing). every on_connect(true) is
     * followed by one on_close. ses->peer_addr is
     * still the server it was connected to, and rpc_clt_send returns -1:
     * the place to deal with the requests that were waiting for a reply */
    void (*on_close)(nw_ses *ses);
} rpc_clt_type;

/* the fields are for reading only */
typedef struct rpc_clt {
    char *name;
    nw_clt *raw_clt;
    nw_timer timer;
    double last_heartbeat;
    void (*on_recv_pkg)(nw_ses *ses, rpc_pkg *pkg);
    void (*on_connect)(nw_ses *ses, bool result);
    void (*on_close)(nw_ses *ses);
} rpc_clt;

/* create a client. return NULL if name or on_recv_pkg is missing or
 * addr_count is 0 */
rpc_clt *rpc_clt_create(rpc_clt_cfg *cfg, rpc_clt_type *type);
/* begin to connect and start the heartbeat. return 0 or -1, see nw_clt_start */
int rpc_clt_start(rpc_clt *clt);
/* close the connection and stay closed until rpc_clt_start */
int rpc_clt_stop(rpc_clt *clt);
/* send a packet, see rpc_send. check rpc_clt_connected first */
int rpc_clt_send(rpc_clt *clt, rpc_pkg *pkg);
/* close and free the client. don't call it from one of its callbacks */
void rpc_clt_release(rpc_clt *clt);
/* is the connection established? */
bool rpc_clt_connected(rpc_clt *clt);

# endif
