/*
 * KNOWN BUG: the accept loop spins when the process is out of fds.
 * on_can_accept() reports EMFILE through on_error and returns, but the
 * pending connection stays in the backlog and the listening fd stays
 * readable, so libev calls back immediately, forever: 100% CPU and an
 * on_error_msg storm for as long as the fd shortage lasts.
 */
# include "common.h"
# include <sys/resource.h>

static int errors;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    if (errors++ == 0)
        fprintf(stderr, "first error: %s\n", msg);
}

int main(void)
{
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    st.on_error_msg = on_error_msg;
    nw_svr *svr = ut_svr("tcp@127.0.0.1:0", &st, 1024, NULL);

    nw_addr_t addr;
    CHECK_EQ(nw_sock_host_addr(svr->svr_list[0].sockfd, &addr), 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    ut_run_for(0.01);                   /* let libev set up its own fds first */

    /* no fd left for accept() */
    struct rlimit rlim;
    CHECK_EQ(getrlimit(RLIMIT_NOFILE, &rlim), 0);
    rlim.rlim_cur = fd + 1;
    CHECK_EQ(setrlimit(RLIMIT_NOFILE, &rlim), 0);

    CHECK_EQ(connect(fd, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    ut_run_for(0.2);
    fprintf(stderr, "one pending connection produced %d accept errors in 0.2s\n", errors);
    CHECK(errors < 100);
    return 0;
}
