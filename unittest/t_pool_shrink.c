/*
 * The memory of a peak is given back: after many connections have each
 * held a read buffer and gone away, the server's buf pool keeps what an
 * idle pool keeps, not one buffer per connection of the peak.
 */
# include <sys/socket.h>
# include "common.h"

# define PEAK 400

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

int main(void)
{
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    nw_svr *svr = ut_svr("tcp@127.0.0.1:0", &type, 64 * 1024, NULL);
    nw_addr_t addr;
    CHECK_EQ(nw_sock_host_addr(svr->svr_list[0].sockfd, &addr), 0);
    CHECK_EQ(svr->buf_pool->keep, 64);

    for (int round = 0; round < 3; ++round) {
        /* every connection sends half a message: the server holds a 64K
         * read buffer for each */
        static int fds[PEAK];
        for (int i = 0; i < PEAK; ++i) {
            fds[i] = socket(AF_INET, SOCK_STREAM, 0);
            CHECK(fds[i] >= 0);
            CHECK_EQ(connect(fds[i], NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
            CHECK_EQ(send(fds[i], "half", 4, 0), 4);
            if (i % 50 == 49)
                ut_run_for(0.01);
        }
        ut_run_for(0.1);
        CHECK_EQ(svr->clt_count, PEAK);
        CHECK_EQ(svr->buf_pool->used, PEAK);
        CHECK_EQ(svr->clt_cache->used, PEAK);

        for (int i = 0; i < PEAK; ++i)
            close(fds[i]);
        ut_run_for(0.2);
        CHECK_EQ(svr->clt_count, 0);
        CHECK_EQ(svr->buf_pool->used, 0);
        CHECK_EQ(svr->clt_cache->used, 0);
        fprintf(stderr, "after a peak of %d: %u bufs kept (%u K)\n", PEAK, svr->buf_pool->free, svr->buf_pool->free * 64);
        CHECK_EQ(svr->buf_pool->free, svr->buf_pool->keep);
        CHECK(svr->clt_cache->free <= svr->clt_cache->keep);
    }

    nw_svr_release(svr);
    return 0;
}
