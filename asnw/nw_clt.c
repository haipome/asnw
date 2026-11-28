/*
 * Description: client, keeps a connection to one of its servers
 *     History: yang@haipo.me, 2016/03/22, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <errno.h>
# include <time.h>
# include <unistd.h>

# include "nw_clt.h"

/*
 * the servers of a client are the entries of clt->addrs, which have their
 * turn one after the other, round and round: addr_next is the one whose
 * turn comes next, addr_curr the one that has it. an entry that is a name
 * is looked up when its turn comes, and its turn lasts until the addresses
 * the lookup gave (resolved) have all been tried.
 *
 * the states of a client, and what tells them apart:
 *
 *   stopped      no socket, no timer. after nw_clt_create and nw_clt_stop
 *   resolving    dns_query is set: the name whose turn it is, is looked up
 *   connecting   a socket whose connect is on its way. timer is the connect
 *                timeout, on_connect_called says that the answer has come
 *   connected    connected is true, on_connect(true) has been called
 *   waiting      no socket. timer brings the next server its turn: at once
 *                after a server that failed, after reconnect_timeout when
 *                they have all failed
 *
 * and how one becomes the other:
 *
 *   nw_clt_start  -> try_next
 *   try_next      -> resolving, or connecting
 *   resolving     -> connecting, or server_failed
 *   connecting    -> connected: on_connect(true)
 *                 -> server_failed, if it was refused or gave no answer
 *                    within reconnect_timeout
 *   server_failed -> on_error_msg, then waiting, for no time at all. but
 *                    if that was the last of addr_count servers to fail in
 *                    a row (addr_failed): on_connect(false), on_close, and
 *                    waiting for reconnect_timeout
 *   connected     -> on_close, then waiting, for no time at all, when the
 *                    peer closes or on an error
 *   waiting       -> try_next, when timer fires
 *   any           -> stopped, by nw_clt_stop: on_close
 *
 * waiting for no time at all, a timer of 0 seconds, takes the next step
 * out of the callback that found the failure: servers that fail on the
 * spot can never make this code call itself.
 *
 * a dgram client has no connecting state: it is connected as soon as it
 * has a socket, an error does not close it, and it stays with the first
 * of its servers.
 *
 * the application may stop or start the client from inside a callback,
 * and the code after the callback has to notice:
 *
 *   generation    counts every nw_clt_start and nw_clt_stop. the code that
 *                 called on_error_msg, on_connect(false) or on_close goes
 *                 on only if it has not changed
 *   in_on_close   on_close is running: a nw_clt_stop from inside it does
 *                 not call it again
 */

static int create_socket(int family, int sock_type)
{
    int sockfd = socket(family, sock_type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0)
        return -1;
    if (sock_type == SOCK_STREAM && (family == AF_INET || family == AF_INET6)) {
        if (nw_sock_set_no_delay(sockfd) < 0) {
            close(sockfd);
            return -1;
        }
    }

    return sockfd;
}

/* best effort: a refused buffer size is not a reason to drop the connection */
static void set_socket_option(nw_clt *clt, int sockfd)
{
    if (clt->read_mem > 0)
        nw_sock_set_recv_buf(sockfd, clt->read_mem);
    if (clt->write_mem > 0)
        nw_sock_set_send_buf(sockfd, clt->write_mem);
}

/* local address of a unix dgram client, unique per process and per start,
 * whichever thread asks. does not touch the application's rand() state */
static void generate_local_path(char *path, size_t size)
{
    static unsigned int counter;
    unsigned int seq = __sync_fetch_and_add(&counter, 1);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(path, size, "%s/asnw_dgram_%d_%u_%09ld.sock", P_tmpdir, (int)getpid(), seq, (long)ts.tv_nsec);
}

static void reconnect_later(nw_clt *clt, double delay);
static int try_next(nw_clt *clt);
static void server_failed(nw_clt *clt, const char *msg);

static void on_retry(nw_timer *timer, void *privdata)
{
    nw_clt *clt = privdata;
    if (try_next(clt) < 0) {
        /* no socket or no lookup to be had: out of fds, of memory ... */
        char msg[100];
        snprintf(msg, sizeof(msg), "connect error: %m");
        server_failed(clt, msg);
    }
}

static void reconnect_later(nw_clt *clt, double delay)
{
    nw_timer_stop(&clt->timer);
    nw_timer_set(&clt->timer, delay, false, on_retry, clt);
    nw_timer_start(&clt->timer);
}

static void on_connect_timeout(nw_timer *timer, void *privdata)
{
    nw_clt *clt = privdata;
    if (clt->on_connect_called)
        return;
    char msg[NW_SOCK_HUMAN_ADDR_SIZE + 64];
    snprintf(msg, sizeof(msg), "connect %s: no answer in %g seconds",
            nw_sock_human_addr(&clt->ses.peer_addr), clt->reconnect_timeout);
    errno = ETIMEDOUT;
    server_failed(clt, msg);
}

static void watch_connect(nw_clt *clt)
{
    nw_timer_stop(&clt->timer);
    nw_timer_set(&clt->timer, clt->reconnect_timeout, false, on_connect_timeout, clt);
    nw_timer_start(&clt->timer);
}

/* a connect that failed on the spot is dealt with on the next turn of the
 * loop, like one whose failure the loop reports */
static void on_failed_at_once(nw_timer *timer, void *privdata)
{
    nw_clt *clt = privdata;
    char msg[NW_SOCK_HUMAN_ADDR_SIZE + 128];
    snprintf(msg, sizeof(msg), "connect %s: %s",
            nw_sock_human_addr(&clt->ses.peer_addr), strerror(clt->failed_errno));
    errno = clt->failed_errno;
    server_failed(clt, msg);
}

static void on_recv_fd(nw_ses *ses, int fd)
{
    close(fd);
}

static int clt_close(nw_clt *clt)
{
    nw_timer_stop(&clt->timer);
    clt->connected = false;
    if (clt->dns_query) {
        nw_dns_cancel(clt->dns_query);
        clt->dns_query = NULL;
    }
    int ret = nw_ses_close(&clt->ses);
    if (clt->local_path_bound) {
        unlink(clt->ses.local_addr->un.sun_path);
        clt->local_path_bound = false;
    }
    return ret;
}

/* tell the application that the connection is over. while it is told the
 * session refuses to send, quietly: a send that failed with an error would
 * bring us right back here for the same connection */
static void call_on_close(nw_clt *clt)
{
    if (clt->type.on_close) {
        clt->in_on_close = true;
        clt->ses.closing = true;
        clt->type.on_close(&clt->ses);
        clt->in_on_close = false;
    }
}

/* the connection that was established is over: tell the application, and
 * give the next server its turn */
static void connection_lost(nw_clt *clt)
{
    if (clt->in_on_close)
        return;
    uint32_t generation = clt->generation;
    call_on_close(clt);
    /* stopped or started again from the callback: the application has
     * taken over, what follows is not ours to do any more */
    if (generation != clt->generation)
        return;
    clt_close(clt);
    reconnect_later(clt, 0);
}

/*
 * the server that had its turn could not be reached. msg says why, for the
 * application; NULL if it has been told. what comes next:
 *
 *   the next address of its name, if a name it is and has more of them,
 *   else the next server of the list,
 *   or, when all of them have failed one after the other, the report that
 *   the attempt has failed, and a wait before the list is gone through
 *   again.
 */
static void server_failed(nw_clt *clt, const char *msg)
{
    if (clt->in_on_close)
        return;
    int why = errno;
    uint32_t generation = clt->generation;
    if (msg && clt->type.on_error_msg) {
        clt->type.on_error_msg(&clt->ses, msg);
        if (generation != clt->generation)
            return;
    }
    clt_close(clt);

    if (clt->ses.sock_type == SOCK_DGRAM) {
        /* nothing to connect to: what failed is a lookup, or the socket.
         * the same again, later */
        clt->resolved_count = 0;
        reconnect_later(clt, clt->reconnect_timeout);
        return;
    }
    if (clt->resolved_next < clt->resolved_count) {
        reconnect_later(clt, 0);
        return;
    }
    clt->resolved_count = 0;
    if (++clt->addr_failed < clt->addr_count) {
        reconnect_later(clt, 0);
        return;
    }

    clt->addr_failed = 0;
    if (clt->type.on_connect) {
        errno = why;
        clt->type.on_connect(&clt->ses, false);
        if (generation != clt->generation)
            return;
    }
    call_on_close(clt);
    if (generation != clt->generation)
        return;
    clt_close(clt);
    reconnect_later(clt, clt->reconnect_timeout);
}

static int connect_to_peer(nw_clt *clt);

static void on_resolved(int error, nw_addr *addrs, uint32_t count, void *privdata)
{
    nw_clt *clt = privdata;
    clt->dns_query = NULL;
    const char *name = clt->addrs[clt->addr_curr].host.name;
    char msg[NW_SOCK_MAX_NAME + 128];
    if (error) {
        snprintf(msg, sizeof(msg), "resolve %s: %s", name, nw_dns_strerror(error));
        errno = EHOSTUNREACH;
        server_failed(clt, msg);
        return;
    }

    /* a dgram client sends to the first, it has no way to find out that
     * another one would be better */
    if (clt->ses.sock_type == SOCK_DGRAM)
        count = 1;
    nw_addr *copy = realloc(clt->resolved, count * sizeof(nw_addr));
    if (copy == NULL) {
        snprintf(msg, sizeof(msg), "resolve %s: out of memory", name);
        errno = ENOMEM;
        server_failed(clt, msg);
        return;
    }
    memcpy(copy, addrs, count * sizeof(nw_addr));
    clt->resolved = copy;
    clt->resolved_count = count;
    clt->resolved_next = 0;
    if (try_next(clt) < 0) {
        snprintf(msg, sizeof(msg), "connect error: %m");
        server_failed(clt, msg);
    }
}

static void on_connect(nw_ses *ses, bool result)
{
    nw_clt *clt = (nw_clt *)ses;
    clt->on_connect_called = true;
    if (!result) {
        int why = errno;
        char msg[NW_SOCK_HUMAN_ADDR_SIZE + 128];
        snprintf(msg, sizeof(msg), "connect %s: %s", nw_sock_human_addr(&ses->peer_addr), strerror(why));
        errno = why;
        server_failed(clt, msg);
        return;
    }

    nw_timer_stop(&clt->timer);
    clt->connected = true;
    clt->addr_failed = 0;
    /* when this connection is lost the next server has its turn, and a
     * name is looked up again: it may point elsewhere by then */
    clt->resolved_count = 0;
    set_socket_option(clt, clt->ses.sockfd);
    nw_sock_local_addr(ses->sockfd, ses->local_addr);
    if (clt->type.on_connect)
        clt->type.on_connect(ses, true);
}

static void on_error(nw_ses *ses, const char *msg)
{
    nw_clt *clt = (nw_clt *)ses;
    uint32_t generation = clt->generation;
    if (clt->type.on_error_msg)
        clt->type.on_error_msg(ses, msg);
    if (generation != clt->generation)
        return;
    if (ses->sock_type == SOCK_DGRAM)
        return;
    if (clt->connected)
        connection_lost(clt);
    else
        server_failed(clt, NULL);
}

static void on_close(nw_ses *ses)
{
    nw_clt *clt = (nw_clt *)ses;
    connection_lost(clt);
}

nw_clt *nw_clt_create(nw_clt_cfg *cfg, nw_clt_type *type, void *privdata)
{
    nw_loop_init();

    if (cfg->addr_count == 0 || cfg->addrs == NULL)
        return NULL;
    if (cfg->max_pkg_size == 0)
        return NULL;
    if (type->decode_pkg == NULL)
        return NULL;
    if (type->on_recv_pkg == NULL)
        return NULL;
    /* a name stands for ipv4 and ipv6 addresses, which are for tcp and udp */
    for (uint32_t i = 0; i < cfg->addr_count; ++i) {
        if (cfg->addrs[i].family == NW_SOCK_AF_NAME && cfg->sock_type != SOCK_STREAM && cfg->sock_type != SOCK_DGRAM)
            return NULL;
    }

    nw_clt *clt = malloc(sizeof(nw_clt));
    if (clt == NULL)
        return NULL;
    memset(clt, 0, sizeof(nw_clt));
    clt->ses.sockfd = -1;
    clt->type = *type;
    clt->reconnect_timeout = cfg->reconnect_timeout  == 0 ? 1.0 : cfg->reconnect_timeout;
    if (cfg->buf_pool) {
        clt->custom_buf_pool = true;
        clt->buf_pool = cfg->buf_pool;
    } else {
        clt->custom_buf_pool = false;
        clt->buf_pool = nw_buf_pool_create(cfg->max_pkg_size);
        if (clt->buf_pool == NULL) {
            nw_clt_release(clt);
            return NULL;
        }
    }
    clt->read_mem = cfg->read_mem;
    clt->write_mem = cfg->write_mem;

    if (nw_ses_init(&clt->ses, nw_default_loop, clt->buf_pool, cfg->buf_limit, NW_SES_TYPE_CLIENT) < 0) {
        nw_clt_release(clt);
        return NULL;
    }
    nw_addr *local_addr = malloc(sizeof(nw_addr));
    if (local_addr == NULL) {
        nw_clt_release(clt);
        return NULL;
    }
    memset(local_addr, 0, sizeof(nw_addr));
    clt->ses.local_addr   = local_addr;
    clt->addrs = malloc(cfg->addr_count * sizeof(nw_addr));
    if (clt->addrs == NULL) {
        nw_clt_release(clt);
        return NULL;
    }
    memcpy(clt->addrs, cfg->addrs, cfg->addr_count * sizeof(nw_addr));
    clt->addr_count = cfg->addr_count;
    if (clt->addrs[0].family != NW_SOCK_AF_NAME) {
        /* known before the client is started, for whoever reads it */
        local_addr->family = clt->addrs[0].family;
        local_addr->addrlen = clt->addrs[0].addrlen;
        memcpy(&clt->ses.peer_addr, &clt->addrs[0], sizeof(nw_addr));
    }
    clt->ses.sockfd      = -1;
    clt->ses.sock_type   = cfg->sock_type;
    clt->ses.privdata    = privdata;

    clt->ses.decode_pkg  = type->decode_pkg;
    clt->ses.on_recv_pkg = type->on_recv_pkg;
    clt->ses.on_recv_fd  = type->on_recv_fd == NULL ? on_recv_fd : type->on_recv_fd;
    clt->ses.on_connect  = on_connect;
    clt->ses.on_error    = on_error;
    clt->ses.on_close    = on_close;
    clt->ses.on_drain    = type->on_drain;

    return clt;
}

int nw_clt_start(nw_clt *clt)
{
    /* starting an already started client restarts it, the old socket and
     * any pending reconnect or lookup are dropped */
    clt->generation++;
    clt_close(clt);
    clt->addr_failed = 0;
    clt->resolved_count = 0;
    return try_next(clt);
}

/* give the next server its turn: the next address of the name that is
 * being gone through, else the next entry of the list, looked up first if
 * it is a name. -1 if nothing could be started, errno says why */
static int try_next(nw_clt *clt)
{
    if (clt->resolved_next < clt->resolved_count) {
        memcpy(&clt->ses.peer_addr, &clt->resolved[clt->resolved_next++], sizeof(nw_addr));
        return connect_to_peer(clt);
    }
    clt->resolved_count = 0;

    /* a dgram client stays with the first: nothing tells it that a server
     * has failed */
    clt->addr_curr = clt->ses.sock_type == SOCK_DGRAM ? 0 : clt->addr_next;
    clt->addr_next = (clt->addr_curr + 1) % clt->addr_count;
    nw_addr *addr = &clt->addrs[clt->addr_curr];
    if (addr->family == NW_SOCK_AF_NAME) {
        clt->on_connect_called = false;
        clt->dns_query = nw_dns_resolve(addr->host.name, addr->host.port, on_resolved, clt);
        if (clt->dns_query == NULL) {
            errno = ENOMEM;
            return -1;
        }
        return 0;
    }
    memcpy(&clt->ses.peer_addr, addr, sizeof(nw_addr));
    return connect_to_peer(clt);
}

/* open a socket and connect it to ses.peer_addr */
static int connect_to_peer(nw_clt *clt)
{
    int sockfd = create_socket(clt->ses.peer_addr.family, clt->ses.sock_type);
    if (sockfd < 0)
        return -1;
    clt->ses.sockfd = sockfd;
    /* the family can change from one server to the next */
    clt->ses.local_addr->family = clt->ses.peer_addr.family;
    clt->ses.local_addr->addrlen = clt->ses.peer_addr.addrlen;
    if (clt->ses.peer_addr.family == AF_UNIX && clt->ses.sock_type == SOCK_DGRAM) {
        nw_addr *local_addr = clt->ses.local_addr;
        memset(local_addr, 0, sizeof(nw_addr));
        local_addr->family = AF_UNIX;
        local_addr->addrlen = sizeof(local_addr->un);
        local_addr->un.sun_family = AF_UNIX;
        generate_local_path(local_addr->un.sun_path, sizeof(local_addr->un.sun_path));
        if (nw_ses_bind(&clt->ses, local_addr) < 0) {
            int bind_errno = errno;
            clt_close(clt);
            errno = bind_errno;
            return -1;
        }
        clt->local_path_bound = true;
    }

    if (clt->ses.sock_type == SOCK_STREAM || clt->ses.sock_type == SOCK_SEQPACKET) {
        clt->connected = false;
        clt->on_connect_called = false;
        if (nw_ses_connect(&clt->ses, &clt->ses.peer_addr) < 0) {
            /* failed on the spot (unix path missing, backlog full ...) */
            clt->failed_errno = errno;
            nw_timer_stop(&clt->timer);
            nw_timer_set(&clt->timer, 0, false, on_failed_at_once, clt);
            nw_timer_start(&clt->timer);
            return 0;
        }
        if (!clt->on_connect_called)
            watch_connect(clt);
        return 0;
    } else {
        clt->connected = true;
        set_socket_option(clt, clt->ses.sockfd);
        nw_sock_local_addr(clt->ses.sockfd, clt->ses.local_addr);
        if (nw_ses_start(&clt->ses) < 0)
            return -1;
        if (clt->resolved_count > 0) {
            /* a dgram client is usable at once, except when its server
             * had to be looked up first: say when it is */
            if (clt->type.on_connect)
                clt->type.on_connect(&clt->ses, true);
        }
        return 0;
    }
}

int nw_clt_stop(nw_clt *clt)
{
    clt->generation++;
    /* from inside on_close: it is being told already */
    if (!clt->in_on_close)
        call_on_close(clt);
    clt->resolved_count = 0;
    return clt_close(clt);
}

void nw_clt_release(nw_clt *clt)
{
    /* stops the connect/reconnect timer: libev must not keep a pointer
     * into the memory freed below */
    clt_close(clt);
    nw_ses_release(&clt->ses);
    if (!clt->custom_buf_pool && clt->buf_pool)
        nw_buf_pool_release(clt->buf_pool);
    free(clt->ses.local_addr);
    free(clt->addrs);
    free(clt->resolved);
    free(clt);
}

bool nw_clt_connected(nw_clt *clt)
{
    return clt->connected;
}
