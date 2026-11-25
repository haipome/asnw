/*
 * Description: example: http server
 *     History: 2026/11/25, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <signal.h>

# include "nw_http.h"

/*
 * try it with:
 *   ./http_server 'tcp@127.0.0.1:8080'
 *   curl -i http://127.0.0.1:8080/
 *   curl -i -d 'some data' 'http://127.0.0.1:8080/echo?a=1'
 */

static void on_request(nw_ses *ses, nw_http_request *req)
{
    printf("%s %s %s%s (%zu bytes) from %s\n", req->method.s, req->path.s,
            req->query.len ? "?" : "", req->query.s, req->body_size, nw_sock_human_addr(&ses->peer_addr));
    for (uint32_t i = 0; i < req->header_count; ++i) {
        printf("    %s: %s\n", req->headers[i].name.s, req->headers[i].value.s);
    }

    if (strcmp(req->path.s, "/") == 0) {
        const char *body = "hello, this is asnw\n";
        nw_http_reply_text(ses, 200, "text/plain", body, strlen(body));
    } else if (strcmp(req->path.s, "/echo") == 0) {
        /* send the request body back, with its content type */
        nw_http_str *type = nw_http_get_header(req, "Content-Type");
        nw_http_response rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.status = 200;
        nw_http_add_header(&rsp, "Content-Type", type ? type->s : "application/octet-stream");
        rsp.body = req->body;
        rsp.body_size = req->body_size;
        nw_http_reply(ses, &rsp);
    } else {
        const char *body = "no such page\n";
        nw_http_reply_text(ses, 404, "text/plain", body, strlen(body));
    }
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    printf("error: %s, peer: %s\n", msg, nw_sock_human_addr(&ses->peer_addr));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: %s bind...\n", argv[0]);
        exit(0);
    }

    int bind_count = argc - 1;
    nw_svr_bind *bind_arr = calloc(bind_count, sizeof(nw_svr_bind));
    if (bind_arr == NULL) {
        printf("malloc fail\n");
        exit(1);
    }
    for (int i = 0; i < bind_count; ++i) {
        int ret = nw_sock_cfg_parse(argv[i + 1], &bind_arr[i].addr, &bind_arr[i].sock_type);
        if (ret < 0) {
            printf("parse bind: %s fail: %d\n", argv[i + 1], ret);
            exit(1);
        }
    }

    nw_http_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = bind_count;
    cfg.bind_arr = bind_arr;

    nw_http_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_request = on_request;
    type.on_error_msg = on_error_msg;

    nw_http_svr *svr = nw_http_svr_create(&cfg, &type, NULL);
    if (svr == NULL) {
        printf("nw_http_svr_create fail\n");
        exit(1);
    }
    if (nw_http_svr_start(svr) < 0) {
        printf("nw_http_svr_start fail\n");
        exit(1);
    }

    printf("http server start\n");
    nw_loop_run();
    printf("http server stop\n");

    return 0;
}
