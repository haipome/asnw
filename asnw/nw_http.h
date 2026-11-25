/*
 * Description: http server, HTTP/1.0 and HTTP/1.1 on top of nw_svr
 *     History: 2026/11/25, create
 */

# ifndef _NW_HTTP_H_
# define _NW_HTTP_H_

# include <stdint.h>
# include <stddef.h>
# include <stdbool.h>
# include <time.h>

# include "nw_buf.h"
# include "nw_svr.h"
# include "nw_timer.h"

/*
 * nw_http_svr is a http server: it receives requests on its tcp or unix
 * stream binds, hands every complete request to on_request and sends the
 * response you give it.
 *
 * usage:
 *   fill a nw_http_svr_cfg and a nw_http_svr_type (memset them to 0 first),
 *   nw_http_svr_create -> nw_http_svr_start -> nw_loop_run
 *   in on_request, fill a nw_http_response and call nw_http_reply
 *
 * see test/http_server.c.
 *
 * reading is zero copy
 *
 * a request is parsed in place: every string of a nw_http_request points
 * into the read buffer of the connection, nothing is allocated or copied.
 * the separators behind them are overwritten with '\0', so method, path,
 * query, header names and header values are ordinary C strings. the price:
 * the request and everything it points to are only valid during
 * on_request. copy what you need for later.
 *
 * replying later
 *
 * to answer after on_request returned (the result comes from a nw_job, from
 * another service ...), keep ses->id and call nw_http_reply_id when the
 * response is ready. it does nothing if the client left in the meantime.
 *
 * what is supported
 *
 *   - HTTP/1.0 and HTTP/1.1, keep-alive
 *   - request bodies with Content-Length or chunked transfer encoding
 *   - Expect: 100-continue, HEAD
 *
 * what is not
 *
 *   - TLS, HTTP/2, websocket. put a reverse proxy in front for those
 *   - a request (head + body) bigger than max_request_size: it has to fit
 *     in one buffer. the answer is 413 or 431
 *   - streamed responses: a response is given as a whole
 *   - pipelining: a request that arrives while the previous one is not
 *     answered yet is dropped and the connection is closed after that
 *     answer
 *   - the absolute form of the request target (GET http://host/path), which
 *     only proxies receive
 *   - clients that shut down their sending side and then wait: the end of
 *     their input closes the connection, an answer given later is lost
 *
 * requests that are malformed or ambiguous about where they end (both
 * Content-Length and Transfer-Encoding, a repeated Content-Length, bare LF
 * line ends, white space before the colon, folded header lines ...) are
 * refused with 400 and the connection is closed, never repaired.
 */

/* a piece of the request. s[len] is always '\0' */
typedef struct nw_http_str {
    char *s;
    uint32_t len;
} nw_http_str;

typedef struct nw_http_header {
    nw_http_str name;
    nw_http_str value;
} nw_http_header;

/* a request or a response can carry this many headers */
# define NW_HTTP_MAX_HEADERS 64

typedef struct nw_http_request {
    /* as sent: GET, POST ... (case sensitive) */
    nw_http_str method;
    /* the part of the target before '?', percent decoded. it is NOT
     * normalized: check for ".." yourself before mapping it to a file */
    nw_http_str path;
    /* the part after '?', as sent (see nw_http_url_decode). empty if none */
    nw_http_str query;
    /* 1 for HTTP/1.1, 0 for HTTP/1.0 */
    int version_minor;
    /* does the client want the connection kept open after the response */
    bool keep_alive;
    /* the headers in the order they were sent, see nw_http_get_header */
    uint32_t header_count;
    nw_http_header headers[NW_HTTP_MAX_HEADERS];
    /* the body, already de-chunked. NOT '\0' terminated, use body_size.
     * body_size is 0 if there is none */
    void *body;
    size_t body_size;
} nw_http_request;

typedef struct nw_http_response {
    /* 200, 404 ... */
    int status;
    /* the text after the status code, NULL for the standard one */
    const char *reason;
    /* set to close the connection after this response even if the client
     * asked for keep-alive */
    bool close;
    /* your headers, see nw_http_add_header. Content-Length, Connection and
     * Date are added for you (a Date of your own replaces the automatic
     * one; Content-Length, Connection and Transfer-Encoding of your own
     * are ignored) */
    uint32_t header_count;
    nw_http_header headers[NW_HTTP_MAX_HEADERS];
    /* the body, may be NULL when body_size is 0 */
    const void *body;
    size_t body_size;
} nw_http_response;

/*
 * request helpers
 */

/* the value of a header, by name, case insensitive. NULL if the request
 * does not have it. if it was sent several times this is the first one,
 * walk req->headers with nw_http_header_is to get them all */
nw_http_str *nw_http_get_header(nw_http_request *req, const char *name);

/* is this header called `name`, case insensitive */
bool nw_http_header_is(nw_http_header *header, const char *name);

/* percent decode len bytes of s in place ("%41" -> "A"; with plus_as_space
 * also "+" -> " ", as in query strings and form bodies). the result is
 * '\0' terminated if it got shorter. return the new length, or -1 if s
 * holds a broken escape */
int nw_http_url_decode(char *s, size_t len, bool plus_as_space);

/*
 * parse one complete request (head and body) of `size` bytes, in place.
 * this is what the server uses, exposed for tests and other transports.
 * return 0 and fill req, whose strings point into data; or the status code
 * the request deserves: 400, 431 (more than NW_HTTP_MAX_HEADERS headers),
 * 501 (a transfer encoding other than chunked), 505 (not HTTP/1.x)
 */
int nw_http_parse_request(char *data, size_t size, nw_http_request *req);

/*
 * response helpers
 */

/* add a header to a response. name and value are NOT copied: they have to
 * stay valid until the response is sent. return 0, or -1 if the response
 * is full */
int nw_http_add_header(nw_http_response *rsp, const char *name, const char *value);

/* the standard text of a status code: 404 -> "Not Found" */
const char *nw_http_status_text(int status);

/*
 * server
 */

typedef struct nw_http_svr_cfg {
    /* number of entries in bind_arr, at least 1 */
    uint32_t bind_count;
    /* the addresses to listen on, all SOCK_STREAM (tcp or unix stream) */
    nw_svr_bind *bind_arr;
    /* max size of a request, head and body together, in bytes. it is the
     * size of the read buffer a connection holds while a request is coming
     * in. 0 for the default, 64K. at most 1G */
    uint32_t max_request_size;
    /* max number of bufs (of max_request_size bytes) of response data
     * waiting to be sent on a connection, 0 for no limit. see nw_svr_cfg */
    uint32_t buf_limit;
    /* seconds an idle connection is kept open. 0 for the default, 60 */
    double keep_alive_timeout;
    /* seconds a client may take to send a whole request, counted from its
     * first byte; 408 is sent after that. 0 for the default, 10 */
    double request_timeout;
} nw_http_svr_cfg;

typedef struct nw_http_svr_type {
    /* must
     *
     * called for every request. req and everything it points to are only
     * valid during the call. answer with nw_http_reply before returning,
     * or keep ses->id and answer later with nw_http_reply_id. a request
     * that is never answered keeps its connection open */
    void (*on_request)(nw_ses *ses, nw_http_request *req);
    /* optional, see nw_svr_type */
    void (*on_new_connection)(nw_ses *ses);
    /* optional, see nw_svr_type */
    void (*on_connection_close)(nw_ses *ses);
    /* optional, see nw_svr_type. refused requests (400, 413 ...) are
     * reported here too */
    void (*on_error_msg)(nw_ses *ses, const char *msg);
} nw_http_svr_type;

/* the fields are for reading only */
typedef struct nw_http_svr {
    /* the nw_svr underneath: raw_svr->clt_count, nw_svr_close_clt ... */
    nw_svr *raw_svr;
    nw_http_svr_type type;
    /* the privdata given to nw_http_svr_create */
    void *privdata;
    uint32_t max_request_size;
    double keep_alive_timeout;
    double request_timeout;
    nw_cache *conn_cache;
    /* sends the pending "100 Continue" */
    nw_timer continue_timer;
    uint64_t *continue_ids;
    uint32_t continue_count;
    uint32_t continue_total;
    /* where a response head is put together */
    char *send_buf;
    size_t send_buf_size;
    /* the Date header, rebuilt once a second */
    time_t date_time;
    char date[40];
} nw_http_svr;

/* create a server. cfg and type are copied. return NULL if on_request is
 * missing, a bind is not SOCK_STREAM, or nw_svr_create fails */
nw_http_svr *nw_http_svr_create(nw_http_svr_cfg *cfg, nw_http_svr_type *type, void *privdata);

/* bind and start accepting. return 0 or -1, see nw_svr_start */
int nw_http_svr_start(nw_http_svr *svr);

/* stop accepting new connections. existing ones are served on */
int nw_http_svr_stop(nw_http_svr *svr);

/* close every connection and free the server. don't call it from a callback
 * of this server */
void nw_http_svr_release(nw_http_svr *svr);

/* the server a session belongs to, for use in the callbacks */
nw_http_svr *nw_http_svr_of(nw_ses *ses);

/*
 * answer the request of a connection. rsp is not modified and nothing of
 * it is kept after the call.
 *
 * return 0 if the response was sent or queued. return -1 if the connection
 * has no request waiting for an answer (it was answered already), a header
 * holds a line break, or sending failed.
 *
 * the connection may be closed by the time this returns (the client did
 * not ask for keep-alive, rsp->close, an error): don't use ses afterwards.
 */
int nw_http_reply(nw_ses *ses, nw_http_response *rsp);

/* the same for a connection known by its id (ses->id saved in on_request),
 * for answering later. return -1 as well if that connection is gone */
int nw_http_reply_id(nw_http_svr *svr, uint64_t id, nw_http_response *rsp);

/* shortcut for a response that is only a status and a body. content_type
 * may be NULL when there is no body */
int nw_http_reply_text(nw_ses *ses, int status, const char *content_type, const void *body, size_t size);

# endif
