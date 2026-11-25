/*
 * Regression (wrapper/ut_rpc.c, fixed): the rpc_decode() length check used
 * to overflow. pkg_size = RPC_PKG_HEAD_SIZE + ext_size + body_size is a
 * uint32_t sum; with body_size close to 4G it wrapped to a small number,
 * passed the RPC_MAX_PKG_SIZE check and - the sender controls the crc too -
 * the packet was accepted, handing the handler body_size = 0xFFFFFFxx with
 * a body pointer into a 1MB buffer.
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
    fprintf(stderr, "rpc_decode(body_size=%u) = %d\n", pkg->body_size, ret);
    CHECK_EQ(ret, -2);

    /* the limits themselves */
    memset(buf, 0, sizeof(buf));
    pkg->magic = RPC_PKG_MAGIC;
    pkg->body_size = RPC_MAX_PKG_SIZE;                      /* head pushes it over */
    CHECK_EQ(rpc_decode(NULL, buf, RPC_PKG_HEAD_SIZE), -2);
    pkg->body_size = RPC_MAX_PKG_SIZE - RPC_PKG_HEAD_SIZE;  /* exactly the maximum: wait for more */
    CHECK_EQ(rpc_decode(NULL, buf, RPC_PKG_HEAD_SIZE), 0);
    pkg->magic = 0;
    CHECK_EQ(rpc_decode(NULL, buf, RPC_PKG_HEAD_SIZE), -1);
    CHECK_EQ(rpc_decode(NULL, buf, RPC_PKG_HEAD_SIZE - 1), 0);

    /* a well formed packet, and the same one with a flipped bit */
    memset(buf, 0, sizeof(buf));
    pkg->magic = RPC_PKG_MAGIC;
    pkg->command = 7;
    pkg->ext_size = 2;
    pkg->body_size = 5;
    memcpy(buf + RPC_PKG_HEAD_SIZE, "exhello", 7);
    pkg->crc32 = generate_crc32c(buf, RPC_PKG_HEAD_SIZE + 7);
    CHECK_EQ(rpc_decode(NULL, buf, sizeof(buf)), RPC_PKG_HEAD_SIZE + 7);
    buf[RPC_PKG_HEAD_SIZE + 3] ^= 1;
    CHECK_EQ(rpc_decode(NULL, buf, sizeof(buf)), -3);
    return 0;
}
