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

    rpc_pkg *pkg = data;
    if (le32toh(pkg->magic) != RPC_PKG_MAGIC)
        return -1;
    /* check body_size on its own first: the sum below is 32 bit and a
     * body_size near 4G would wrap around to a small, valid looking size */
    uint32_t body_size = le32toh(pkg->body_size);
    if (body_size > RPC_MAX_PKG_SIZE)
        return -2;
    uint32_t pkg_size = RPC_PKG_HEAD_SIZE + le16toh(pkg->ext_size) + body_size;
    if (pkg_size > RPC_MAX_PKG_SIZE)
        return -2;
    if (max < pkg_size)
        return 0;

    uint32_t crc32 = le32toh(pkg->crc32);
    pkg->crc32 = 0;
    if (crc32 != generate_crc32(data, pkg_size))
        return -3;
    pkg->crc32 = crc32;

    pkg->magic     = le32toh(pkg->magic);
    pkg->command   = le32toh(pkg->command);
    pkg->pkg_type  = le16toh(pkg->pkg_type);
    pkg->ext_size  = le16toh(pkg->ext_size);
    pkg->result    = le32toh(pkg->result);
    pkg->sequence  = le32toh(pkg->sequence);
    pkg->req_id    = le64toh(pkg->req_id);
    pkg->body_size = le32toh(pkg->body_size);
    pkg->resv      = le32toh(pkg->resv);

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

    memcpy(send_buf, pkg, RPC_PKG_HEAD_SIZE);
    if (pkg->ext_size)
        memcpy(send_buf + RPC_PKG_HEAD_SIZE, pkg->ext, pkg->ext_size);
    if (pkg->body_size)
        memcpy(send_buf + RPC_PKG_HEAD_SIZE + pkg->ext_size, pkg->body, pkg->body_size);

    pkg = send_buf;
    pkg->magic     = htole32(RPC_PKG_MAGIC);
    pkg->command   = htole32(pkg->command);
    pkg->pkg_type  = htole16(pkg->pkg_type);
    pkg->ext_size  = htole16(pkg->ext_size);
    pkg->result    = htole32(pkg->result);
    pkg->sequence  = htole32(pkg->sequence);
    pkg->req_id    = htole64(pkg->req_id);
    pkg->body_size = htole32(pkg->body_size);
    pkg->resv      = 0;

    pkg->crc32 = 0;
    pkg->crc32 = htole32(generate_crc32(send_buf, pkg_size));

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
