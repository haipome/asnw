/*
 * Description: ut_rpc, a simple rpc protocol on top of asnw: packet format
 *     History: yang@haipo.me, 2016/03/30, create
 */

# include <stdlib.h>
# include <string.h>
# include <time.h>
# include <assert.h>
# include <endian.h>
# include <sys/time.h>
# include <unistd.h>

# include "ut_rpc.h"
# include "ut_crc32.h"

/* htole16/32/64 and le16/32/64toh come from <endian.h> */

int rpc_decode(nw_ses *ses, void *data, size_t max)
{
    if (max < RPC_PKG_HEAD_SIZE)
        return 0;

    /* a packet starts wherever the one before it ended, at any address:
     * the header is worked on in a copy, never through a rpc_pkg pointer
     * into the buffer, which would have to be aligned */
    rpc_pkg head;
    memcpy(&head, data, RPC_PKG_HEAD_SIZE);
    if (le32toh(head.magic) != RPC_PKG_MAGIC)
        return -1;
    /* check body_size on its own first: the sum below is 32 bit and a
     * body_size near 4G would wrap around to a small, valid looking size */
    uint32_t body_size = le32toh(head.body_size);
    if (body_size > RPC_MAX_PKG_SIZE)
        return -2;
    uint32_t pkg_size = RPC_PKG_HEAD_SIZE + le16toh(head.ext_size) + body_size;
    if (pkg_size > RPC_MAX_PKG_SIZE)
        return -2;
    if (max < pkg_size)
        return 0;

    /* the checksum is that of the packet with 0 in its place */
    uint32_t crc32 = le32toh(head.crc32);
    memset(data + offsetof(rpc_pkg, crc32), 0, sizeof(head.crc32));
    if (crc32 != generate_crc32(data, pkg_size))
        return -3;

    /* the header goes back in host byte order */
    head.magic     = le32toh(head.magic);
    head.command   = le32toh(head.command);
    head.pkg_type  = le16toh(head.pkg_type);
    head.ext_size  = le16toh(head.ext_size);
    head.result    = le32toh(head.result);
    head.crc32     = crc32;
    head.sequence  = le32toh(head.sequence);
    head.req_id    = le64toh(head.req_id);
    head.body_size = le32toh(head.body_size);
    head.resv      = le32toh(head.resv);
    memcpy(data, &head, RPC_PKG_HEAD_SIZE);

    return pkg_size;
}

int rpc_send(nw_ses *ses, rpc_pkg *pkg)
{
    if (pkg->body_size > RPC_MAX_PKG_SIZE)
        return -1;
    uint32_t pkg_size = RPC_PKG_HEAD_SIZE + pkg->ext_size + pkg->body_size;
    if (pkg_size > RPC_MAX_PKG_SIZE)
        return -1;

    /* the packet is put together on the stack, a big one on the heap */
    char small_buf[65536];
    void *send_buf = small_buf;
    if (pkg_size > sizeof(small_buf)) {
        send_buf = malloc(pkg_size);
        if (send_buf == NULL)
            return -1;
    }

    /* the header as it goes on the wire, with 0 for the checksum */
    rpc_pkg head = {
        .magic     = htole32(RPC_PKG_MAGIC),
        .command   = htole32(pkg->command),
        .pkg_type  = htole16(pkg->pkg_type),
        .ext_size  = htole16(pkg->ext_size),
        .result    = htole32(pkg->result),
        .sequence  = htole32(pkg->sequence),
        .req_id    = htole64(pkg->req_id),
        .body_size = htole32(pkg->body_size),
    };

    memcpy(send_buf, &head, RPC_PKG_HEAD_SIZE);
    if (pkg->ext_size)
        memcpy(send_buf + RPC_PKG_HEAD_SIZE, pkg->ext, pkg->ext_size);
    if (pkg->body_size)
        memcpy(send_buf + RPC_PKG_HEAD_SIZE + pkg->ext_size, pkg->body, pkg->body_size);

    head.crc32 = htole32(generate_crc32(send_buf, pkg_size));
    memcpy(send_buf + offsetof(rpc_pkg, crc32), &head.crc32, sizeof(head.crc32));

    int ret = nw_ses_send(ses, send_buf, pkg_size);
    if (send_buf != small_buf)
        free(send_buf);
    return ret;
}

double rpc_monotonic_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

double current_timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1000000.0;
}
