/*
 * KNOWN BUG: nw_svr_release() frees svr->clt_cache twice.
 * nw_svr_release calls nw_cache_release(svr->clt_cache), then nw_svr_free(svr)
 * releases the same cache again (nw_svr.c). Every clean server shutdown hits it.
 */
# include "common.h"

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

int main(void)
{
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    nw_svr *svr = ut_svr("tcp@127.0.0.1:0", &type, 1024, NULL);
    nw_svr_release(svr);
    return 0;
}
