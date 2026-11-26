/*
 * A unix socket path that is already there when a server starts. Left
 * behind by a run that is over, it is removed and the server starts. With
 * a server still listening on it, it used to be removed all the same
 * (regression): the second server started, new connections went to it, the
 * first one kept its old ones and nobody was told. Now the second fails
 * to start, as it does on a tcp port that is taken. Anything that is not
 * a socket is left alone, as before.
 */
# include <fcntl.h>
# include <sys/socket.h>
# include <sys/stat.h>
# include <sys/un.h>
# include "common.h"

static int got;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    got++;
    nw_ses_send(ses, data, size);
}

static int whole(nw_ses *ses, void *data, size_t max)
{
    return max;
}

static nw_svr *server(const char *cfg_str)
{
    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_addr_parse(cfg_str, &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    cfg.max_pkg_size = 1024;
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = whole;
    type.on_recv_pkg = on_recv_pkg;
    nw_svr *svr = nw_svr_create(&cfg, &type, NULL);
    CHECK(svr != NULL);
    return svr;
}

/* does a server answer on the path? */
static bool answers(const char *path, int sock_type)
{
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    int fd = socket(AF_UNIX, sock_type, 0);
    CHECK(fd >= 0);
    int before = got;
    bool ok = false;
    if (sock_type == SOCK_DGRAM) {
        ok = sendto(fd, "ping", 4, 0, (struct sockaddr *)&un, sizeof(un)) == 4;
    } else if (connect(fd, (struct sockaddr *)&un, sizeof(un)) == 0) {
        ok = send(fd, "ping", 4, 0) == 4;
    }
    if (ok) {
        for (int i = 0; i < 200 && got == before; ++i)
            ut_run_for(0.005);
        ok = got == before + 1;
    }
    close(fd);
    ut_run_for(0.01);
    return ok;
}

static void check(const char *proto, int sock_type)
{
    char cfg[128], name[32];
    snprintf(name, sizeof(name), "bind-%s.sock", proto);
    ut_unix_cfg(cfg, sizeof(cfg), proto, name);
    const char *path = strchr(cfg, '@') + 1;
    struct stat st;

    nw_svr *first = server(cfg);
    CHECK_EQ(nw_svr_start(first), 0);
    CHECK(answers(path, sock_type));

    /* a second server on the same path: it does not start, and the first
     * one has lost nothing */
    nw_svr *second = server(cfg);
    errno = 0;
    CHECK_EQ(nw_svr_start(second), -1);
    CHECK_EQ(errno, EADDRINUSE);
    nw_svr_release(second);
    CHECK_EQ(lstat(path, &st), 0);
    CHECK(answers(path, sock_type));
    int before = got;
    CHECK(answers(path, sock_type));
    CHECK_EQ(got, before + 1);

    /* the first one is gone and its path is still there: a leftover, which
     * the next server removes */
    nw_svr_release(first);
    CHECK_EQ(lstat(path, &st), 0);
    CHECK(S_ISSOCK(st.st_mode));
    CHECK(!answers(path, sock_type));
    nw_svr *third = server(cfg);
    CHECK_EQ(nw_svr_start(third), 0);
    CHECK(answers(path, sock_type));
    nw_svr_release(third);

    /* not a socket: never removed, and the server does not start */
    CHECK_EQ(unlink(path), 0);
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    CHECK_EQ(write(fd, "data", 4), 4);
    close(fd);
    nw_svr *fourth = server(cfg);
    CHECK_EQ(nw_svr_start(fourth), -1);
    nw_svr_release(fourth);
    CHECK_EQ(lstat(path, &st), 0);
    CHECK(S_ISREG(st.st_mode));
    CHECK_EQ(st.st_size, 4);
    CHECK_EQ(unlink(path), 0);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    check("stream", SOCK_STREAM);
    check("dgram", SOCK_DGRAM);
    check("seqpacket", SOCK_SEQPACKET);
    nw_loop_release();
    return 0;
}
