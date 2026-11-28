/*
 * Description: client, keeps a connection to one of its servers
 *     History: yang@haipo.me, 2016/03/21, create
 */

# ifndef _NW_CLT_H_
# define _NW_CLT_H_

# include <stdint.h>
# include <stdbool.h>

# include "nw_buf.h"
# include "nw_loop.h"
# include "nw_ses.h"
# include "nw_dns.h"
# include "nw_sock.h"
# include "nw_timer.h"

/*
 * nw_clt is a client: it keeps a connection to a server and connects again
 * by itself whenever the connection is lost or can't be established.
 *
 * usage:
 *   fill a nw_clt_cfg (the servers via nw_sock_addr_parse) and a
 *   nw_clt_type (callbacks, memset it to 0 first), then
 *   nw_clt_create -> nw_clt_start -> nw_loop_run ... -> nw_clt_release
 *   send with nw_ses_send(&clt->ses, data, size)
 *
 * all callbacks run on the loop thread. see example/ping_clt.c.
 *
 * several servers
 *
 * the cfg holds a list of servers that can stand in for each other, one
 * or more. they have their turn one after the other, round and round: the
 * client connects to the first; when that fails, or when the connection
 * is lost later, it goes on to the next one at once, and after the last
 * one to the first again. a server that can't be reached is reported
 * through on_error_msg, and that is all. only when every server of the
 * list has failed, one after the other, has the attempt failed: on_connect
 * with false, on_close, and reconnect_timeout seconds later the client
 * goes on where it stopped. the order of the list is the order of the
 * attempts, the client has no preference of its own.
 *
 * a server given by name
 *
 * an entry of the list may be the name of a host with a port instead of
 * an address: what nw_sock_addr_parse makes of "tcp@db.local:5432". the
 * name is looked up by nw_dns, off the loop thread, each time its turn
 * comes: a client follows its server when the name moves to another
 * address. a name with several addresses has them tried one by one within
 * its turn, in the order the system gives them, and has failed when the
 * last of them has; a name that can't be resolved has failed as well. the
 * callbacks see the address in use in ses->peer_addr.
 *
 * stream and seqpacket clients
 *
 * nw_clt_start only begins to connect, on_connect tells how it ended. do
 * not send before the client is connected (check nw_clt_connected): the
 * failed write is treated as a connection error. data still waiting to be
 * sent when a connection is lost is dropped.
 *
 * dgram clients
 *
 * there is no connection, and so nothing that could fail and make the
 * client go on to another server: it uses the first entry of the list and
 * no other. it is usable right after nw_clt_start, on_connect is not
 * called (and on_close only by nw_clt_stop), and nothing tells whether the
 * server is there. if that entry is a name, the name is looked up once,
 * when the client is started: the client can't send before the lookup is
 * done, on_connect(true) tells when, and it sends to the first address of
 * the name until nw_clt_start is called again, wherever the name points by
 * then. a lookup that fails is reported through on_error_msg only, and
 * repeated reconnect_timeout later.
 * udp datagrams arriving from any other address than the one it sends to
 * are dropped, unless that is a broadcast, multicast or wildcard address.
 * a unix dgram client binds a socket file of its own under /tmp so that
 * the server can reply, and removes it on close.
 */

typedef struct nw_clt_cfg {
    /* number of entries in addrs, at least 1 */
    uint32_t addr_count;
    /* the servers, in the order they have their turn: addresses, and for
     * tcp and udp names of hosts as well, both as nw_sock_addr_parse gives
     * them. copied by nw_clt_create */
    nw_addr *addrs;
    /* SOCK_STREAM, SOCK_DGRAM or SOCK_SEQPACKET, for all of them */
    int sock_type;
    /* must not be 0
     *
     * max size of one message, in bytes, and the size of the bufs used for
     * reading and for the pending send list. with buf_pool set, the buf
     * size of that pool is what applies (this must still not be 0) */
    uint32_t max_pkg_size;
    /* max number of bufs in the pending send list, i.e. up to
     * buf_limit * max_pkg_size bytes can wait to be sent; nw_ses_send fails
     * beyond that. 0 means no limit */
    uint32_t buf_limit;
    /* kernel receive buffer size of the socket (SO_RCVBUF), 0 to keep the
     * system default */
    uint32_t read_mem;
    /* kernel send buffer size of the socket (SO_SNDBUF), 0 to keep the
     * system default */
    uint32_t write_mem;
    /* seconds to wait before going through the servers again when all of
     * them have failed. also the time one of them has to answer a connect
     * before it is given up. 0 means the default, 1.0 */
    double reconnect_timeout;
    /* optional
     *
     * a buf pool to use instead of a private one, to share bufs between
     * many clients. its buf size takes the place of max_pkg_size. it must
     * outlive the client and is not released with it */
    nw_buf_pool *buf_pool;
} nw_clt_cfg;

typedef struct nw_clt_type {
    /* must
     *
     * tell where a message ends, same contract as nw_svr_type.decode_pkg:
     *
     * return > 0: the size of the first full message in data,
     * return = 0: no full message yet (stream: wait for more data;
     *             dgram, seqpacket: the packet is dropped),
     * return < 0: broken data (stream, seqpacket: the connection is closed
     *             and connected again; dgram: only reported). */
    int (*decode_pkg)(nw_ses *ses, void *data, size_t max);
    /* optional
     *
     * called once each time the connection is lost, an attempt to connect
     * has failed (after on_connect with false), or nw_clt_stop is called.
     * the place to fail the requests that were waiting for a reply.
     *
     * the connection is over when this is called: nw_ses_send returns -1.
     * the client goes on by itself afterwards, unless nw_clt_stop is
     * called from here: that means "stay closed" */
    void (*on_close)(nw_ses *ses);
    /* optional
     *
     * called when everything that was queued has been written to the
     * socket, see on_drain in nw_svr_type */
    void (*on_drain)(nw_ses *ses);
    /* optional
     *
     * called when an attempt to connect ends. result true: connected, the
     * client can send, ses->peer_addr tells to which server. result
     * false: none of the servers could be reached, on_error_msg has said
     * why for each of them and errno holds the reason of the last;
     * on_close is called next and the client tries again later.
     * a dgram client is only told, with true, when its server was given by
     * name: the lookup is done */
    void (*on_connect)(nw_ses *ses, bool result);
    /* must
     *
     * called for every full message, put your business logic here. data is
     * only valid during the call, copy what you need to keep */
    void (*on_recv_pkg)(nw_ses *ses, void *data, size_t size);
    /* optional
     *
     * called when a file descriptor arrives on a seqpacket connection.
     * the fd is yours, close it when done. if not set, it is closed */
    void (*on_recv_fd)(nw_ses *ses, int fd);
    /* optional
     *
     * called when something goes wrong, msg says what, for your log: a
     * server that can't be reached, with its address, or an error on the
     * connection, which is over then (stream, seqpacket: on_close is
     * called next). msg is only valid during the call */
    void (*on_error_msg)(nw_ses *ses, const char *msg);
} nw_clt_type;

/* the fields are for reading only */
typedef struct nw_clt {
    /* the session, pass &clt->ses to nw_ses_send. must stay the first
     * member: the nw_ses given to the callbacks is this one */
    nw_ses ses;
    nw_clt_type type;
    bool custom_buf_pool;
    nw_buf_pool *buf_pool;
    /* connect timeout while connecting, reconnect delay while waiting */
    nw_timer timer;
    bool connected;
    bool on_connect_called;
    /* a unix dgram client binds a private socket file, removed on close */
    bool local_path_bound;
    /* the servers of the cfg. addr_next is the one whose turn comes next,
     * addr_curr the one that has it, addr_failed counts those that have
     * failed one after the other since the last connection */
    nw_addr *addrs;
    uint32_t addr_count;
    uint32_t addr_next;
    uint32_t addr_curr;
    uint32_t addr_failed;
    /* a server given by name: the lookup in progress if any, and the
     * addresses it gave, of which resolved_next is the next one to try */
    nw_dns_query *dns_query;
    nw_addr *resolved;
    uint32_t resolved_count;
    uint32_t resolved_next;
    /* errno of a connect that failed on the spot, until the loop turns */
    int failed_errno;
    /* on_close is running */
    bool in_on_close;
    /* changes each time the client is started or stopped. a callback that
     * did either has taken over: what the library was doing for the
     * connection it reported is dropped */
    uint32_t generation;
    double reconnect_timeout;
    uint32_t read_mem;
    uint32_t write_mem;
} nw_clt;

/* create a client. cfg and type are copied. nothing is connected yet.
 * privdata is stored in clt->ses.privdata, i.e. it is ses->privdata in
 * every callback. return NULL if there is no server, max_pkg_size is 0, a
 * `must` callback is missing, a server is given by name and sock_type is
 * neither SOCK_STREAM nor SOCK_DGRAM, or memory runs out */
nw_clt *nw_clt_create(nw_clt_cfg *cfg, nw_clt_type *type, void *privdata);

/* start the client: begin to connect (stream, seqpacket) or get ready to
 * send (dgram). on a client that is already started it drops the current
 * connection, without a call to on_close, and starts over. the first
 * start begins with the first server; a later one goes on with the server
 * whose turn is next, like the client itself does.
 * return 0 once the client is under way: also when the first attempt
 * failed on the spot, since it goes on by itself (no callback is called
 * from inside nw_clt_start for a failure). return -1 if no socket could
 * be created or bound, or the lookup of a name could not be started;
 * nothing is retried then */
int nw_clt_start(nw_clt *clt);

/* close the connection and stay closed: on_close is called, pending data
 * is dropped and no reconnect is scheduled. nw_clt_start starts it again.
 * it may be called from any callback of the client, with the same result:
 * on_close once, and nothing more happens for the connection or the
 * attempt the callback was called for */
int nw_clt_stop(nw_clt *clt);

/* close without calling on_close, and free the client. don't call it from
 * a callback of this client */
void nw_clt_release(nw_clt *clt);

/* can the client send right now? stream, seqpacket: the connection is
 * established. dgram: the client is started and, if its server was given
 * by name, the lookup is done */
bool nw_clt_connected(nw_clt *clt);

# endif
