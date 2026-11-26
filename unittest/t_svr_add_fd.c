/*
 * nw_svr_add_fd: a connection made elsewhere becomes a connection of the
 * server, of the bind of its kind. A server that listens on udp and on tcp
 * used to make a tcp connection a session of its udp bind, the first of
 * the family, and then treated the stream as datagrams (regression).
 */
# include <sys/socket.h>
# include <netinet/in.h>
# include "common.h"

static int opens, seen_type, real_type, echoes;

static void on_open(nw_ses *ses)
{
    socklen_t len = sizeof(real_type);
    CHECK_EQ(getsockopt(ses->sockfd, SOL_SOCKET, SO_TYPE, &real_type, &len), 0);
    seen_type = ses->sock_type;
    opens++;
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    echoes++;
    nw_ses_send(ses, data, size);
}

/* a tcp connection of our own making: the client end, and the accepted one */
static void tcp_pair(int *client, int *accepted)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(lfd >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(in);
    CHECK_EQ(bind(lfd, (struct sockaddr *)&in, len), 0);
    CHECK_EQ(listen(lfd, 4), 0);
    CHECK_EQ(getsockname(lfd, (struct sockaddr *)&in, &len), 0);
    *client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(*client >= 0);
    CHECK_EQ(connect(*client, (struct sockaddr *)&in, sizeof(in)), 0);
    *accepted = accept(lfd, NULL, NULL);
    CHECK(*accepted >= 0);
    close(lfd);
}

static nw_svr *server(const char *first, const char *second)
{
    static nw_svr_bind binds[2];
    memset(binds, 0, sizeof(binds));
    CHECK_EQ(nw_sock_addr_parse(first, &binds[0].addr, &binds[0].sock_type), 0);
    if (second)
        CHECK_EQ(nw_sock_addr_parse(second, &binds[1].addr, &binds[1].sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = second ? 2 : 1;
    cfg.binds = binds;
    cfg.max_pkg_size = 1024;
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_open = on_open;
    nw_svr *svr = nw_svr_create(&cfg, &type, NULL);
    CHECK(svr != NULL);
    return svr;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    int client, accepted;
    char buf[64];

    /* udp first, tcp second: the tcp connection is a stream */
    nw_svr *svr = server("udp@127.0.0.1:0", "tcp@127.0.0.1:0");
    tcp_pair(&client, &accepted);
    CHECK_EQ(nw_svr_add_fd(svr, accepted), 0);
    ut_run_for(0.02);
    CHECK_EQ(opens, 1);
    CHECK_EQ(real_type, SOCK_STREAM);
    CHECK_EQ(seen_type, SOCK_STREAM);
    CHECK_EQ(svr->ses_count, 1);
    /* and works as one: two messages in one write, one message in two */
    CHECK_EQ(send(client, "one\ntwo\nthr", 11, 0), 11);
    ut_run_for(0.02);
    CHECK_EQ(echoes, 2);
    CHECK_EQ(send(client, "ee\n", 3, 0), 3);
    ut_run_for(0.02);
    CHECK_EQ(echoes, 3);
    CHECK_EQ(recv(client, buf, sizeof(buf), MSG_DONTWAIT), 14);
    CHECK(memcmp(buf, "one\ntwo\nthree\n", 14) == 0);
    close(client);
    ut_run_for(0.02);
    CHECK_EQ(svr->ses_count, 0);
    nw_svr_release(svr);

    /* no bind of its kind: refused, and the fd is still the caller's */
    svr = server("udp@127.0.0.1:0", NULL);
    tcp_pair(&client, &accepted);
    CHECK_EQ(nw_svr_add_fd(svr, accepted), -1);
    CHECK_EQ(svr->ses_count, 0);
    CHECK_EQ(send(accepted, "mine\n", 5, 0), 5);
    CHECK_EQ(recv(client, buf, sizeof(buf), 0), 5);
    close(accepted);
    close(client);
    nw_svr_release(svr);

    nw_loop_release();
    return 0;
}
