/*
 * KNOWN BUG (wrapper/ut_rpc.c): rpc_decode() length check overflows.
 * pkg_size = RPC_PKG_HEAD_SIZE + ext_size + body_size is computed in
 * uint32_t. With body_size close to 4G the sum wraps to a small number,
 * passes the RPC_MAX_PKG_SIZE check and - since the sender controls the
 * crc too - the packet is accepted. The handler is then given
 * pkg->body_size = 0xFFFFFFxx with a body pointer into a 1MB buffer:
 * an unauthenticated peer gets an out-of-bounds read in whatever code
 * consumes the body.
 */
# include "common.h"
# include "ut_rpc.h"
# include "ut_crc32.h"

int main(void)
{
    char buf[256];
    memset(buf, 0, sizeof(buf));
    rpc_pkg *pkg = (rpc_pkg *)buf;
    pkg->magic = RPC_PKG_MAGIC;
    pkg->command = 100;
    pkg->body_size = 0xFFFFFFF0u;       /* head(36) + 0 + this wraps to 20 */
    pkg->crc32 = 0;
    uint32_t wrapped = (uint32_t)(RPC_PKG_HEAD_SIZE + pkg->body_size);
    pkg->crc32 = generate_crc32c(buf, wrapped);

    int ret = rpc_decode(NULL, buf, RPC_PKG_HEAD_SIZE);
    fprintf(stderr, "rpc_decode accepted body_size=%u as a %d byte packet\n", pkg->body_size, ret);
    CHECK(ret < 0);
    return 0;
}
