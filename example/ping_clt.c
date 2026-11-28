/*
 * Description: example: nw_clt sending a ping every second, to one of
 *              the servers it is given: addresses or names of hosts
 *              ping_clt 'tcp@127.0.0.1:1234'
 *              ping_clt 'tcp@a.local:1234' 'tcp@b.local:1234' 
 *     History: yang@haipo.me, 2016/03/22, create
 */

# include <stdio.h>
# include <errno.h>
# include <stdlib.h>
# include <unistd.h>
# include <errno.h>
# include <error.h>

# include "nw_clt.h"
# include "nw_timer.h"

int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    char *s = data;
    for (size_t i = 0; i < max; ++i) {
        if (s[i] == '\n')
            return i + 1;
    }
    return 0;
}

void on_connect(nw_ses *ses, bool result)
{
    if (result) {
        printf("connect to: %s success\n", nw_sock_human_addr(&ses->peer_addr));
    } else {
        /* on_error_msg has said why for each of them */
        printf("none of the servers could be reached\n");
    }
}

void on_close(nw_ses *ses)
{
    printf("connection to: %s close\n", nw_sock_human_addr(&ses->peer_addr));
}

void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    char *str = malloc(size + 1);
    memcpy(str, data, size);
    str[size] = 0;
    printf("from: %s recv: %zu: %s", nw_sock_human_addr(&ses->peer_addr), size, str);
    free(str);
}

void on_error_msg(nw_ses *ses, const char *msg)
{
    printf("error: %s, peer: %s\n", msg, nw_sock_human_addr(&ses->peer_addr));
}

void on_timeout(nw_timer *timer, void *privdata)
{
    nw_clt *clt = privdata;
    if (nw_clt_connected(clt)) {
        char *msg = "ping\n";
        nw_ses_send(&clt->ses, msg, strlen(msg));
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: %s addr [addr ...]\n", argv[0]);
        exit(0);
    }

    /* the servers have their turn in the order they are given */
    nw_clt_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_pkg_size = 10240;
    cfg.addr_count = argc - 1;
    cfg.addrs = calloc(cfg.addr_count, sizeof(nw_addr));
    for (int i = 1; i < argc; ++i) {
        int sock_type;
        if (nw_sock_addr_parse(argv[i], &cfg.addrs[i - 1], &sock_type) < 0)
            error(1, 0, "parse peer addr: %s fail", argv[i]);
        if (i > 1 && sock_type != cfg.sock_type)
            error(1, 0, "%s: not the same kind of socket as %s", argv[i], argv[1]);
        cfg.sock_type = sock_type;
    }

    nw_clt_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg   = decode_pkg;
    type.on_connect   = on_connect;
    type.on_close     = on_close;
    type.on_recv_pkg  = on_recv_pkg;
    type.on_error_msg = on_error_msg;

    nw_clt *clt = nw_clt_create(&cfg, &type, NULL);
    if (clt == NULL) {
        error(1, errno, "nw_clt_create fail");
    }
    /* the client has its own copy */
    free(cfg.addrs);
    nw_clt_start(clt);

    nw_timer timer;
    nw_timer_set(&timer, 1.0, true, on_timeout, clt);
    nw_timer_start(&timer);

    nw_loop_run();

    return 0;
}
