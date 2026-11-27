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

/* a well formed packet that does not start at a multiple of 8, as the
 * second packet in a read buffer does when the first has an odd size.
 * rpc_decode used to read the header through a rpc_pkg pointer, which is
 * only allowed for an aligned address once the struct is no longer packed
 * (regression, caught by UBSan). */
static void test_unaligned(void)
{
    char store[256];
    memset(store, 0, sizeof(store));
    for (int shift = 1; shift < 8; ++shift) {
        char *buf = store + shift;
        rpc_pkg head;
        memset(&head, 0, sizeof(head));
        head.magic = RPC_PKG_MAGIC;
        head.command = 7;
        head.sequence = 0x01020304;
        head.req_id = 0x1122334455667788ull;
        head.body_size = 5;
        memcpy(buf, &head, RPC_PKG_HEAD_SIZE);
        memcpy(buf + RPC_PKG_HEAD_SIZE, "hello", 5);
        head.crc32 = generate_crc32(buf, RPC_PKG_HEAD_SIZE + 5);
        memcpy(buf, &head, RPC_PKG_HEAD_SIZE);

        CHECK_EQ(rpc_decode(NULL, buf, RPC_PKG_HEAD_SIZE + 5), RPC_PKG_HEAD_SIZE + 5);
        rpc_pkg got;
        memcpy(&got, buf, RPC_PKG_HEAD_SIZE);
        CHECK_EQ(got.command, 7);
        CHECK_EQ(got.sequence, 0x01020304);
        CHECK(got.req_id == 0x1122334455667788ull);
        CHECK_EQ(got.body_size, 5);
        CHECK(got.crc32 == head.crc32);
    }
}

int main(void)
{
    test_unaligned();

    _Alignas(rpc_pkg) char buf[256];
    memset(buf, 0, sizeof(buf));
    rpc_pkg *pkg = (rpc_pkg *)buf;
    pkg->magic = RPC_PKG_MAGIC;
    pkg->command = 100;
    pkg->body_size = 0xFFFFFFF0u;       /* head(40) + 0 + this wraps to 24 */
    pkg->crc32 = 0;
    uint32_t wrapped = (uint32_t)(RPC_PKG_HEAD_SIZE + pkg->body_size);
    pkg->crc32 = generate_crc32(buf, wrapped);

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
    pkg->crc32 = generate_crc32(buf, RPC_PKG_HEAD_SIZE + 7);
    CHECK_EQ(rpc_decode(NULL, buf, sizeof(buf)), RPC_PKG_HEAD_SIZE + 7);
    buf[RPC_PKG_HEAD_SIZE + 3] ^= 1;
    CHECK_EQ(rpc_decode(NULL, buf, sizeof(buf)), -3);
    return 0;
}
