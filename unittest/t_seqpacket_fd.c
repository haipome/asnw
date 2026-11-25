/* passing a file descriptor over a unix seqpacket connection */
# include "common.h"

static char got[64];
static int got_fd_count;

static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void svr_on_recv_fd(nw_ses *ses, int fd)
{
    got_fd_count++;
    ssize_t n = read(fd, got, sizeof(got) - 1);
    CHECK(n > 0);
    got[n] = 0;
    close(fd);
    nw_loop_break();
}

static void clt_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void clt_on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    char path[128];
    snprintf(path, sizeof(path), "%s/payload", ut_tmpdir());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK_EQ(write(fd, "hello fd", 8), 8);
    CHECK_EQ(lseek(fd, 0, SEEK_SET), 0);
    CHECK(nw_ses_send_fd(ses, fd) > 0);
    close(fd);
}

int main(void)
{
    char cfg[256];
    ut_unix_cfg(cfg, sizeof(cfg), "seqpacket", "fd.sock");

    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = svr_on_recv_pkg;
    st.on_recv_fd = svr_on_recv_fd;
    ut_svr(cfg, &st, 1024, NULL);

    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_connect = clt_on_connect;
    ct.on_recv_pkg = clt_on_recv_pkg;
    nw_clt *clt = ut_clt(cfg, &ct, 1024, 1.0, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);

    /* nw_ses_send_fd is seqpacket only */
    nw_ses fake;
    memset(&fake, 0, sizeof(fake));
    fake.sockfd = 0;
    fake.sock_type = SOCK_STREAM;
    CHECK_EQ(nw_ses_send_fd(&fake, 0), -1);

    ut_run_for(5.0);
    CHECK_EQ(got_fd_count, 1);
    CHECK(strcmp(got, "hello fd") == 0);

    return 0;
}
