/*
 * A peer that has sent all it had to and waits for the answer. With tcp a
 * client says so with shutdown(SHUT_WR): the server reads the end of the
 * stream, and the client goes on reading. What was still to be sent to it
 * used to be dropped there, the session being closed at once (regression:
 * a response of two megabytes arrived as a hundred kilobytes).
 */
# include <fcntl.h>
# include <sys/socket.h>
# include <sys/un.h>
# include <netinet/in.h>
# include "common.h"
# include "nw_http.h"

# define BIG    (2 * 1024 * 1024)
# define CHUNK  (64 * 1024)
# define CHUNKS 40
# define PACKETS 3000

static nw_svr *svr, *seq;
static char *big;
static int opens, closes, errors, drains;
static uint64_t stream_id;
static int stream_left;

static void on_open(nw_ses *ses)
{
    opens++;
}

static void on_close(nw_ses *ses)
{
    closes++;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    errors++;
    fprintf(stderr, "server: %s\n", msg);
}

static void stream_more(nw_ses *ses)
{
    while (stream_left > 0 && nw_ses_pending(ses) == 0) {
        stream_left--;
        CHECK_EQ(nw_ses_send(ses, big, CHUNK), 0);
    }
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    if (size == 4 && memcmp(data, "big\n", 4) == 0) {
        CHECK_EQ(nw_ses_send(ses, big, BIG), 0);
    } else if (size == 4 && memcmp(data, "bye\n", 4) == 0) {
        /* the last thing the server says: it closes when it is out */
        CHECK_EQ(nw_ses_send(ses, big, BIG), 0);
        nw_svr_shutdown(svr, ses);
    } else if (size == 7 && memcmp(data, "stream\n", 7) == 0) {
        /* piece by piece: as much as the socket takes now, the rest from
         * on_drain when what had to wait has gone out */
        stream_id = ses->id;
        stream_left = CHUNKS;
        stream_more(ses);
    }
}

static void on_drain(nw_ses *ses)
{
    drains++;
    if (ses->id == stream_id)
        stream_more(ses);
}

static void seq_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    char packet[1000];
    memset(packet, 'p', sizeof(packet));
    for (int i = 0; i < PACKETS; ++i)
        CHECK_EQ(nw_ses_send(ses, packet, sizeof(packet)), 0);
}

static void on_request(nw_ses *ses, nw_http_request *req)
{
    CHECK_EQ(nw_http_reply_body(ses, 200, "application/octet-stream", big, BIG), 0);
}

static int tcp_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    in.sin_port = htons(port);
    CHECK_EQ(connect(fd, (struct sockaddr *)&in, sizeof(in)), 0);
    return fd;
}

/* read until the end of the stream, or until `want` bytes have come when
 * no end is to be expected. the loop runs meanwhile */
static size_t read_all(int fd, size_t want, bool *ended)
{
    static char buf[65536];
    size_t total = 0;
    *ended = false;
    CHECK_EQ(fcntl(fd, F_SETFL, O_NONBLOCK), 0);
    for (int i = 0; i < 20000; ++i) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            total += n;
            if (want && total >= want)
                break;
        } else if (n == 0) {
            *ended = true;
            break;
        } else {
            CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
            ut_run_for(0.001);
        }
    }
    return total;
}

static void wait_closes(int count)
{
    for (int i = 0; i < 2000 && closes < count; ++i)
        ut_run_for(0.001);
    CHECK_EQ(closes, count);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    big = malloc(BIG);
    for (int i = 0; i < BIG; ++i)
        big[i] = (char)(i * 13 + i / 255);
    bool ended;

    /* a small send buffer, so that most of an answer has to wait */
    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    cfg.max_pkg_size = 1024;
    cfg.write_mem = 8 * 1024;
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    st.on_open = on_open;
    st.on_close = on_close;
    st.on_error_msg = on_error_msg;
    st.on_drain = on_drain;
    svr = nw_svr_create(&cfg, &st, NULL);
    CHECK(svr != NULL);
    CHECK_EQ(nw_svr_start(svr), 0);
    int port = ut_svr_port(svr, 0);

    /* as before: a client that stays gets it all, and the connection stays */
    int fd = tcp_to(port);
    CHECK_EQ(send(fd, "big\n", 4, 0), 4);
    CHECK_EQ(read_all(fd, BIG, &ended), BIG);
    CHECK(!ended);
    CHECK_EQ(closes, 0);
    close(fd);
    wait_closes(1);

    /* the client says it is done and waits: it gets it all, and then the
     * end. for the server this is an end like any other, not an error */
    fd = tcp_to(port);
    CHECK_EQ(send(fd, "big\n", 4, 0), 4);
    CHECK_EQ(shutdown(fd, SHUT_WR), 0);
    static char got[BIG];
    {
        size_t total = 0;
        CHECK_EQ(fcntl(fd, F_SETFL, O_NONBLOCK), 0);
        for (int i = 0; i < 20000; ++i) {
            ssize_t n = recv(fd, got + total, BIG - total + 1 > 65536 ? 65536 : BIG - total, 0);
            if (n > 0)
                total += n;
            else if (n == 0 || total == BIG)
                break;
            else
                ut_run_for(0.001);
        }
        CHECK_EQ(total, BIG);
        CHECK(memcmp(got, big, BIG) == 0);
    }
    CHECK_EQ(read_all(fd, 0, &ended), 0);
    CHECK(ended);
    close(fd);
    wait_closes(2);
    CHECK_EQ(errors, 0);

    /* the same when the server was shutting the connection down anyway */
    fd = tcp_to(port);
    CHECK_EQ(send(fd, "bye\n", 4, 0), 4);
    CHECK_EQ(shutdown(fd, SHUT_WR), 0);
    CHECK_EQ(read_all(fd, 0, &ended), BIG);
    CHECK(ended);
    close(fd);
    wait_closes(3);

    /* and when the answer is made piece by piece from on_drain: the
     * session lives as long as the callback has something to add */
    fd = tcp_to(port);
    CHECK_EQ(send(fd, "stream\n", 7, 0), 7);
    CHECK_EQ(shutdown(fd, SHUT_WR), 0);
    CHECK_EQ(read_all(fd, 0, &ended), (size_t)CHUNKS * CHUNK);
    CHECK(ended);
    CHECK_EQ(stream_left, 0);
    close(fd);
    wait_closes(4);
    CHECK_EQ(errors, 0);

    /* the client said it was done, and then left without reading: the
     * rest cannot be sent. the session is closed, and that is not an
     * error either */
    fd = tcp_to(port);
    CHECK_EQ(send(fd, "big\n", 4, 0), 4);
    CHECK_EQ(shutdown(fd, SHUT_WR), 0);
    ut_run_for(0.05);
    CHECK_EQ(closes, 4);                /* waiting for it to read */
    CHECK_EQ(svr->ses_count, 1);
    close(fd);
    wait_closes(5);
    CHECK_EQ(errors, 0);
    CHECK_EQ(svr->ses_count, 0);

    /* nothing to send: the end of the stream closes the session at once */
    fd = tcp_to(port);
    CHECK_EQ(send(fd, "nothing\n", 8, 0), 8);
    CHECK_EQ(shutdown(fd, SHUT_WR), 0);
    wait_closes(6);
    CHECK_EQ(read_all(fd, 0, &ended), 0);
    CHECK(ended);
    close(fd);
    CHECK_EQ(errors, 0);
    CHECK_EQ(opens, 6);

    /* http: a client that sends its request and half closes */
    static nw_svr_bind http_bind;
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &http_bind.addr, &http_bind.sock_type), 0);
    nw_http_svr_cfg hc;
    memset(&hc, 0, sizeof(hc));
    hc.bind_count = 1;
    hc.binds = &http_bind;
    hc.write_mem = 8 * 1024;
    nw_http_svr_type ht;
    memset(&ht, 0, sizeof(ht));
    ht.on_request = on_request;
    nw_http_svr *http = nw_http_svr_create(&hc, &ht, NULL);
    CHECK(http != NULL);
    CHECK_EQ(nw_http_svr_start(http), 0);
    fd = tcp_to(ut_svr_port(http->raw_svr, 0));
    const char *request = "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
    CHECK_EQ(send(fd, request, strlen(request), 0), (ssize_t)strlen(request));
    CHECK_EQ(shutdown(fd, SHUT_WR), 0);
    size_t total = read_all(fd, 0, &ended);
    CHECK(ended);
    CHECK(total > BIG && total < BIG + 1024);           /* the head and the two megabytes */
    close(fd);
    nw_http_svr_release(http);

    /* packets: a unix seqpacket peer that shuts its side down gets the
     * packets that were waiting for it, all of them */
    char path[128];
    ut_unix_cfg(path, sizeof(path), "seqpacket", "half.sock");
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = seq_on_recv_pkg;
    st.on_close = on_close;
    st.on_error_msg = on_error_msg;
    seq = ut_svr(path, &st, 1024, NULL);
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    CHECK(fd >= 0);
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", strchr(path, '@') + 1);
    CHECK_EQ(connect(fd, (struct sockaddr *)&un, sizeof(un)), 0);
    CHECK_EQ(send(fd, "go\n", 3, 0), 3);
    CHECK_EQ(shutdown(fd, SHUT_WR), 0);
    CHECK_EQ(fcntl(fd, F_SETFL, O_NONBLOCK), 0);
    int packets = 0;
    ended = false;
    for (int i = 0; i < 20000 && !ended; ++i) {
        char packet[2000];
        ssize_t n = recv(fd, packet, sizeof(packet), 0);
        if (n == 1000)
            packets++;
        else if (n == 0)
            ended = true;
        else
            ut_run_for(0.001);
    }
    CHECK(ended);
    CHECK_EQ(packets, PACKETS);
    close(fd);
    wait_closes(7);
    CHECK_EQ(errors, 0);

    nw_svr_release(seq);
    nw_svr_release(svr);
    nw_loop_release();
    free(big);
    return 0;
}
