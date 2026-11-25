/*
 * Description: ut_rpc, a simple rpc protocol on top of asnw: packet format
 *     History: yang@haipo.me, 2016/03/29, create
 */

# ifndef _UT_RPC_H_
# define _UT_RPC_H_

# include <stdint.h>
# include "nw_ses.h"

/*
 * ut_rpc is an example of wrapping nw_svr and nw_clt into something higher
 * level. it adds:
 *   - a packet format: fixed header, optional ext data, body, checksum
 *   - heartbeats, so that both ends notice a dead connection
 *   - a client that fails over between several server addresses
 * see ut_rpc_svr.h, ut_rpc_clt.h, test_rpc_svr.c and test_rpc_clt.c.
 *
 * a packet on the wire, all integers little endian:
 *
 *   | header, RPC_PKG_HEAD_SIZE bytes | ext, ext_size bytes | body, body_size bytes |
 *
 * the content of ext and body is up to the application.
 */

/* max size of a whole packet: header + ext + body */
# define RPC_MAX_PKG_SIZE (1024*1024)
/* first 4 bytes of every packet */
# define RPC_PKG_MAGIC 0x70656562

/* pkg_type. the library does not interpret it, it is a convention between
 * the two ends: a request expects a reply carrying the same sequence, a
 * push is sent by the server on its own and expects nothing back */
# define RPC_PKG_TYPE_REQUEST 0
# define RPC_PKG_TYPE_REPLY   1
# define RPC_PKG_TYPE_PUSH    2

/* the header as it is on the wire (packed), followed by two pointers that
 * exist in memory only */
# pragma pack(1)
typedef struct rpc_pkg {
    /* RPC_PKG_MAGIC, filled in by rpc_send */
    uint32_t magic;
    /* what to do, defined by the application. 0 is the heartbeat */
    uint32_t command;
    /* RPC_PKG_TYPE_REQUEST, RPC_PKG_TYPE_REPLY or RPC_PKG_TYPE_PUSH */
    uint16_t pkg_type;
    /* result code of a reply, defined by the application */
    uint32_t result;
    /* checksum of the whole packet, filled in by rpc_send and verified by
     * rpc_decode */
    uint32_t crc32;
    /* set by the requester and copied into the reply, to match the two */
    uint32_t sequence;
    /* an id for the application, e.g. to trace a request across services */
    uint64_t req_id;
    /* size of body in bytes */
    uint32_t body_size;
    /* size of ext in bytes */
    uint16_t ext_size;
    /* not on the wire. to send: point them at your data. on receive: they
     * point into the read buffer and are only valid during on_recv_pkg */
    void   * ext;
    void   * body;
} rpc_pkg;
# pragma pack()

/* size of the header on the wire */
# define RPC_PKG_HEAD_SIZE (sizeof(rpc_pkg) - sizeof(void *) * 2)

/*
 * the decode_pkg callback for nw_svr / nw_clt. checks magic, size and
 * checksum and converts the header to host byte order in place.
 * return the packet size, 0 if the packet is not complete yet, or
 *   -1: bad magic
 *   -2: bigger than RPC_MAX_PKG_SIZE
 *   -3: checksum mismatch
 */
int rpc_decode(nw_ses *ses, void *data, size_t max);

/*
 * send a packet. set command, pkg_type, result, sequence, req_id, ext,
 * ext_size, body and body_size (memset the rpc_pkg to 0 first); magic and
 * crc32 are filled in. pkg is not modified, ext and body are copied.
 * return 0 on success, -1 if the packet is bigger than RPC_MAX_PKG_SIZE or
 * nw_ses_send failed (see there). call it on the thread ses belongs to.
 */
int rpc_send(nw_ses *ses, rpc_pkg *pkg);

/* wall clock, seconds since the epoch */
double current_timestamp(void);
/* monotonic clock for measuring intervals (heartbeat): not affected by the
 * system time being changed */
double rpc_monotonic_time(void);

/*
 * heartbeat, on stream and seqpacket connections: the client sends a
 * RPC_CMD_HEARTBEAT request every RPC_HEARTBEAT_INTERVAL seconds and the
 * server sends it back. a side that sees no heartbeat for
 * RPC_HEARTBEAT_TIMEOUT seconds closes the connection (the client then
 * reconnects). heartbeats are handled inside ut_rpc and never reach the
 * application callbacks.
 */
# define RPC_HEARTBEAT_INTERVAL 1.0
# define RPC_HEARTBEAT_TIMEOUT  3.0

/* reserved command, don't use 0 for your own packets */
# define RPC_CMD_HEARTBEAT 0

# endif
