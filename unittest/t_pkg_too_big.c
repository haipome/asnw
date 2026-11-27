/*
 * A datagram or a packet bigger than max_pkg_size. The system hands over
 * its beginning and throws the rest away; that beginning used to be given
 * to the application as if it were the whole message. It is now dropped,
 * with a word to on_error_msg; for a seqpacket connection it is an error
 * that closes the connection, as a too big message on a stream is.
 */
# include <sys/socket.h>
# include <sys/un.h>
# include <netinet/in.h>
# include "common.h"

# define MAX 16

static int got, errors, closes, clt_got, clt_errors;
static size_t got_size, clt_got_size;
static char last_error[128];

static int whole(nw_ses *ses, void *data, size_t max)
{
    return max;
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    got++;
    got_size = size;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    errors++;
    snprintf(last_error, sizeof(last_error), "%s", msg);
}

static void on_close(nw_ses *ses)
{
    closes++;
}

static void clt_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    clt_got++;
    clt_got_size = size;
}

static void clt_on_error_msg(nw_ses *ses, const char *msg)
{
    clt_errors++;
}

static void settle(void)
{
    ut_run_for(0.03);
}

int main(void)
{
    const char *letters = "abcdefghijklmnopqrstuvwxyz";
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = whole;
    st.on_recv_pkg = on_recv_pkg;
    st.on_error_msg = on_error_msg;
    st.on_close = on_close;

    /* udp */
    nw_svr *udp = ut_svr("udp@127.0.0.1:0", &st, MAX, NULL);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    in.sin_port = htons(ut_svr_port(udp, 0));
    CHECK_EQ(sendto(fd, letters, 26, 0, (struct sockaddr *)&in, sizeof(in)), 26);
    settle();
    CHECK_EQ(got, 0);                           /* not its first 16 bytes */
    CHECK_EQ(errors, 1);
    CHECK(strstr(last_error, "26 bytes") != NULL);
    fprintf(stderr, "udp: %s\n", last_error);
    /* one byte too many, exactly as many as allowed, less: the server is
     * still there for the ones that fit */
    CHECK_EQ(sendto(fd, letters, MAX + 1, 0, (struct sockaddr *)&in, sizeof(in)), MAX + 1);
    settle();
    CHECK_EQ(got, 0);
    CHECK_EQ(errors, 2);
    CHECK_EQ(sendto(fd, letters, MAX, 0, (struct sockaddr *)&in, sizeof(in)), MAX);
    settle();
    CHECK_EQ(got, 1);
    CHECK_EQ(got_size, MAX);
    CHECK_EQ(sendto(fd, letters, 5, 0, (struct sockaddr *)&in, sizeof(in)), 5);
    settle();
    CHECK_EQ(got, 2);
    CHECK_EQ(got_size, 5);
    CHECK_EQ(errors, 2);
    close(fd);
    nw_svr_release(udp);

    /* a udp client: what its server sends it is held to the same */
    int peer = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(peer >= 0);
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(in);
    CHECK_EQ(bind(peer, (struct sockaddr *)&in, len), 0);
    CHECK_EQ(getsockname(peer, (struct sockaddr *)&in, &len), 0);
    char cfg[128];
    snprintf(cfg, sizeof(cfg), "udp@127.0.0.1:%d", ntohs(in.sin_port));
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = whole;
    ct.on_recv_pkg = clt_on_recv_pkg;
    ct.on_error_msg = clt_on_error_msg;
    nw_clt *clt = ut_clt(cfg, &ct, MAX, 1.0, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    CHECK_EQ(nw_ses_send(&clt->ses, "hi", 2), 0);
    struct sockaddr_in from;
    len = sizeof(from);
    char buf[64];
    CHECK_EQ(recvfrom(peer, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len), 2);
    CHECK_EQ(sendto(peer, letters, 26, 0, (struct sockaddr *)&from, len), 26);
    settle();
    CHECK_EQ(clt_got, 0);
    CHECK_EQ(clt_errors, 1);
    CHECK_EQ(sendto(peer, letters, 10, 0, (struct sockaddr *)&from, len), 10);
    settle();
    CHECK_EQ(clt_got, 1);
    CHECK_EQ(clt_got_size, 10);
    CHECK(nw_clt_connected(clt));
    nw_clt_release(clt);
    close(peer);

    /* unix datagrams */
    got = errors = 0;
    ut_unix_cfg(cfg, sizeof(cfg), "dgram", "big.sock");
    nw_svr *dgram = ut_svr(cfg, &st, MAX, NULL);
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", strchr(cfg, '@') + 1);
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    CHECK_EQ(sendto(fd, letters, 26, 0, (struct sockaddr *)&un, sizeof(un)), 26);
    settle();
    CHECK_EQ(got, 0);
    CHECK_EQ(errors, 1);
    CHECK_EQ(sendto(fd, letters, MAX, 0, (struct sockaddr *)&un, sizeof(un)), MAX);
    settle();
    CHECK_EQ(got, 1);
    CHECK_EQ(got_size, MAX);
    close(fd);
    nw_svr_release(dgram);

    /* seqpacket: a packet that does not fit ends the connection, as a
     * message too big for a stream does. one that fits is a message */
    got = errors = closes = 0;
    ut_unix_cfg(cfg, sizeof(cfg), "seqpacket", "big-seq.sock");
    nw_svr *seq = ut_svr(cfg, &st, MAX, NULL);
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", strchr(cfg, '@') + 1);
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    CHECK(fd >= 0);
    CHECK_EQ(connect(fd, (struct sockaddr *)&un, sizeof(un)), 0);
    CHECK_EQ(send(fd, letters, MAX, 0), MAX);
    settle();
    CHECK_EQ(got, 1);
    CHECK_EQ(got_size, MAX);
    CHECK_EQ(send(fd, letters, 26, 0), 26);
    settle();
    CHECK_EQ(got, 1);
    CHECK_EQ(errors, 1);
    CHECK_EQ(closes, 1);
    CHECK_EQ(seq->ses_count, 0);
    CHECK_EQ(recv(fd, buf, sizeof(buf), 0), 0);         /* closed by the server */
    close(fd);
    nw_svr_release(seq);

    nw_loop_release();
    return 0;
}
