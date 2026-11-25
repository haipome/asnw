/*
 * Description: server, listens on one or more addresses
 *     History: yang@haipo.me, 2016/03/20, create
 */

# ifndef _NW_SVR_H_
# define _NW_SVR_H_

# include <stdint.h>

# include "nw_buf.h"
# include "nw_dict.h"
# include "nw_evt.h"
# include "nw_ses.h"
# include "nw_sock.h"

/*
 * nw_svr is a server: it listens on one or more addresses, which may be of
 * different types (tcp, udp, unix stream / dgram / seqpacket), and handles
 * all of them with the same set of callbacks.
 *
 * usage:
 *   fill a nw_svr_cfg (addresses via nw_sock_cfg_parse) and a nw_svr_type
 *   (callbacks, memset it to 0 first), then
 *   nw_svr_create -> nw_svr_start -> nw_loop_run ... -> nw_svr_release
 *
 * all callbacks run on the loop thread. see test/echo_server.c.
 *
 * session lifetime
 *
 * a nw_ses passed to a callback is only valid until that connection is
 * closed. the memory is recycled for later connections, so a nw_ses pointer
 * kept across asynchronous work (nw_job, nw_state, a timer ...) may by then
 * belong to a different client.
 *
 * so don't keep the pointer, keep ses->id: ids are unique per server, never
 * 0 and never reused. when the work is done, ask for the connection again:
 *
 *   nw_ses *ses = nw_svr_get_clt(svr, saved_id);
 *   if (ses == NULL)
 *       return;        (the client is gone)
 *   nw_ses_send(ses, ...);
 *
 * or use nw_svr_send(svr, saved_id, ...), which does the same.
 *
 * datagrams
 *
 * a dgram bind has no connections: on_new_connection / on_connection_close
 * are not called and no privdata is allocated. the nw_ses in on_recv_pkg is
 * the bound session itself, and nw_ses_send replies to the sender of the
 * datagram being processed; reply inside the callback, or copy
 * ses->peer_addr if you need it later.
 */

typedef struct nw_svr_bind {
    /* the address to listen on */
    nw_addr_t addr;
    /* SOCK_STREAM, SOCK_DGRAM or SOCK_SEQPACKET */
    int sock_type;
} nw_svr_bind;

typedef struct nw_svr_cfg {
    /* number of entries in bind_arr, at least 1 */
    uint32_t bind_count;
    /* the addresses to listen on. copied by nw_svr_create */
    nw_svr_bind *bind_arr;
    /* must not be 0
     *
     * max size of one message, in bytes. it is also the size of the bufs:
     * every connection with unfinished input holds one, and the pending
     * send list is made of them. a stream connection sending a message
     * bigger than this is closed */
    uint32_t max_pkg_size;
    /* max number of bufs in the pending send list of a connection, i.e. a
     * connection can have up to buf_limit * max_pkg_size bytes waiting to
     * be sent; nw_ses_send fails beyond that.
     * 0 means no limit: a peer that stops reading can then hold any amount
     * of memory. set a limit for servers exposed to untrusted clients */
    uint32_t buf_limit;
    /* kernel receive buffer size of each socket (SO_RCVBUF), 0 to keep the
     * system default */
    uint32_t read_mem;
    /* kernel send buffer size of each socket (SO_SNDBUF), 0 to keep the
     * system default */
    uint32_t write_mem;
} nw_svr_cfg;

typedef struct nw_svr_type {
    /* must
     *
     * tell where a message ends. data points to `max` bytes received and
     * not consumed yet.
     *
     * return > 0: the size of the first full message, on_recv_pkg is then
     *             called with exactly that many bytes,
     * return = 0: the data does not hold a full message yet,
     * return < 0: broken data.
     *
     * a stream has no message boundaries, so this is where your framing
     * lives (a length header, a delimiter ...). it is called again for the
     * rest of the data, and again when more arrives. 0 waits for more data;
     * < 0 closes the connection.
     *
     * for dgram and seqpacket every packet is one message, max is the
     * packet size and you normally return it after checking the content.
     * a packet for which 0 is returned is dropped; < 0 closes a seqpacket
     * connection and is only reported (on_error_msg) for dgram.
     *
     * must not close the session or send on it */
    int (*decode_pkg)(nw_ses *ses, void *data, size_t max);
    /* optional
     *
     * called for every connection accepted on a stream or seqpacket bind,
     * in place of the default action, which is to add the connection to the
     * server. ses is the listening session, sockfd the new connection.
     * return < 0 and sockfd is closed for you; otherwise sockfd is yours:
     * hand it to another process (nw_ses_send_fd), or close it.
     * see test/listener_worker.c */
    int (*on_accept)(nw_ses *ses, int sockfd, nw_addr_t *peer_addr);
    /* optional
     *
     * called when a connection has been added to the server and can be
     * used. ses->id, ses->peer_addr and ses->privdata are set */
    void (*on_new_connection)(nw_ses *ses);
    /* optional
     *
     * called when a connection is about to be closed, whatever the reason:
     * the peer closed, an error, nw_svr_close_clt or nw_svr_release. ses is
     * still readable but must not be used to send. the last callback for
     * that connection */
    void (*on_connection_close)(nw_ses *ses);
    /* must
     *
     * called for every full message, put your business logic here. data is
     * only valid during the call, copy what you need to keep. the callback
     * may send, and may close the connection (nw_svr_close_clt) */
    void (*on_recv_pkg)(nw_ses *ses, void *data, size_t size);
    /* optional
     *
     * called when a file descriptor arrives on a seqpacket connection.
     * the fd is yours, close it when done. if not set, it is closed */
    void (*on_recv_fd)(nw_ses *ses, int fd);
    /* optional
     *
     * called when something goes wrong, msg says what, for your log.
     * for a connection (read / write error, broken data, send list full) it
     * is followed by on_connection_close and the connection is closed. for
     * a listening or dgram session (accept or sendto error) the server
     * keeps running. msg is only valid during the call */
    void (*on_error_msg)(nw_ses *ses, const char *msg);
    /* optional, if set on_privdata_free must be set too
     *
     * called for every new connection, before on_new_connection. the
     * return value becomes ses->privdata: the place for your per connection
     * state. svr is the nw_svr. returning NULL rejects the connection */
    void *(*on_privdata_alloc)(void *svr);
    /* optional
     *
     * called when a connection is closed, after on_connection_close, to
     * free what on_privdata_alloc returned */
    void (*on_privdata_free)(void *svr, void *privdata);
} nw_svr_type;

/* the fields are for reading only */
typedef struct nw_svr {
    /* the listening / bound sessions, one per bind, in cfg order */
    uint32_t svr_count;
    nw_ses *svr_list;
    nw_svr_type type;
    nw_buf_pool *buf_pool;
    nw_cache *clt_cache;
    /* the current connections by id, see nw_svr_get_clt */
    nw_dict *clt_dict;
    /* the current connections, linked through nw_ses next / prev. take
     * `next` before closing an entry while walking the list */
    nw_ses *clt_list_head;
    nw_ses *clt_list_tail;
    /* number of current connections */
    uint32_t clt_count;
    uint32_t buf_limit;
    uint32_t read_mem;
    uint32_t write_mem;
    uint64_t id_start;
    /* the privdata given to nw_svr_create. reach it from a callback with
     * ((nw_svr *)ses->svr)->privdata */
    void *privdata;
} nw_svr;

/* create a server. cfg and type are copied. the sockets are created here
 * but nothing is bound yet. privdata is stored in nw_svr privdata.
 * return NULL if cfg or type is incomplete (no bind, max_pkg_size 0, a
 * `must` callback missing), a socket can't be created or memory runs out */
nw_svr *nw_svr_create(nw_svr_cfg *cfg, nw_svr_type *type, void *privdata);

/* bind every address and start accepting / receiving. return 0, or -1 with
 * errno set if an address can't be bound (in use, no permission ...); the
 * binds before the failing one are then already active */
int nw_svr_start(nw_svr *svr);

/* stop accepting new connections and receiving datagrams. existing
 * connections keep working. a stopped server can't be started again */
int nw_svr_stop(nw_svr *svr);

/* close every connection (on_connection_close and on_privdata_free are
 * called for each), close the listening sockets and free the server.
 * don't call it from a callback of this server */
void nw_svr_release(nw_svr *svr);

/* close a connection from the server side. on_connection_close and
 * on_privdata_free are called. ses is invalid afterwards. may be called
 * from a callback, including for the session being handled */
void nw_svr_close_clt(nw_svr *svr, nw_ses *ses);

/* find a current connection by its id (ses->id). return NULL if there is no
 * such connection: it was closed, or the id never existed. this is the safe
 * way to get back to a connection after asynchronous work, see the session
 * lifetime note above. the result is valid until that connection is closed,
 * don't keep it either */
nw_ses *nw_svr_get_clt(nw_svr *svr, uint64_t id);

/* send to a connection by id: nw_svr_get_clt + nw_ses_send. return 0, or -1
 * if the connection is gone or nw_ses_send failed (see there) */
int nw_svr_send(nw_svr *svr, uint64_t id, const void *data, size_t size);

/* add an already connected socket to the server as if it had been accepted
 * here, e.g. a fd received from another process in on_recv_fd. the server
 * must have a bind of the same address family. return 0, or -1 on failure,
 * in which case fd is still yours to close */
int nw_svr_add_clt_fd(nw_svr *svr, int fd);

# endif
