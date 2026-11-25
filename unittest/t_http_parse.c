/*
 * nw_http_parse_request: what is accepted and how it is cut up in place,
 * what is refused and with which status, and that no input - truncated,
 * mutated or random - makes the parser touch memory outside the request.
 */
# include "common.h"
# include "nw_http.h"

/* an exact size heap copy: AddressSanitizer catches any access beyond it */
static char *copy_of(const char *s, size_t size)
{
    char *data = malloc(size ? size : 1);
    memcpy(data, s, size);
    return data;
}

static int status_of(const char *s)
{
    size_t size = strlen(s);
    char *data = copy_of(s, size);
    nw_http_request req;
    int ret = nw_http_parse_request(data, size, &req);
    free(data);
    return ret;
}

# define EXPECT(status, request) do { \
    int _ret = status_of(request); \
    if (_ret != (status)) { \
        fprintf(stderr, "%s:%d: expected %d, got %d for: %s\n", __FILE__, __LINE__, status, _ret, #request); \
        exit(1); \
    } \
} while (0)

static uint32_t rnd_state = 12345;

static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1103515245 + 12345;
    return rnd_state >> 8;
}

int main(void)
{
    nw_http_request req;

    /* a plain GET: everything is a C string pointing into the buffer */
    const char *get = "GET /a%20b/c.txt?x=1&y=%32 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent:  curl/8.5.0  \r\n"
        "X-Empty:\r\n"
        "accept: */*\r\n"
        "\r\n";
    size_t size = strlen(get);
    char *data = copy_of(get, size);
    CHECK_EQ(nw_http_parse_request(data, size, &req), 0);
    CHECK(strcmp(req.method.s, "GET") == 0);
    CHECK_EQ(req.method.len, 3);
    CHECK(strcmp(req.path.s, "/a b/c.txt") == 0);       /* percent decoded */
    CHECK_EQ(req.path.len, 10);
    CHECK(strcmp(req.query.s, "x=1&y=%32") == 0);       /* left as sent */
    CHECK_EQ(req.query.len, 9);
    CHECK_EQ(req.version_minor, 1);
    CHECK(req.keep_alive);
    CHECK_EQ(req.header_count, 4);
    CHECK_EQ(req.body_size, 0);
    CHECK(strcmp(req.headers[0].name.s, "Host") == 0);
    CHECK(strcmp(req.headers[0].value.s, "example.com") == 0);
    CHECK(strcmp(req.headers[1].value.s, "curl/8.5.0") == 0);   /* white space trimmed */
    CHECK_EQ(req.headers[1].value.len, 10);
    CHECK(strcmp(req.headers[2].value.s, "") == 0);
    CHECK_EQ(req.headers[2].value.len, 0);
    /* every string lies inside the request: nothing was copied */
    CHECK(req.method.s == data);
    CHECK(req.path.s > data && req.path.s < data + size);
    CHECK(req.headers[3].value.s > data && req.headers[3].value.s < data + size);
    /* lookup ignores case, in both directions */
    CHECK(nw_http_get_header(&req, "host") == &req.headers[0].value);
    CHECK(nw_http_get_header(&req, "ACCEPT") == &req.headers[3].value);
    CHECK(nw_http_get_header(&req, "Accept-Encoding") == NULL);
    CHECK(nw_http_get_header(&req, "Hos") == NULL);
    CHECK(nw_http_header_is(&req.headers[1], "user-agent"));
    CHECK(!nw_http_header_is(&req.headers[1], "user"));
    /* the query can be decoded in place by the caller */
    CHECK_EQ(nw_http_url_decode(req.query.s, req.query.len, true), 7);
    CHECK(strcmp(req.query.s, "x=1&y=2") == 0);
    free(data);

    /* a POST with a body: the body is a pointer and a size */
    const char *post = "POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\nContent-Type: text/plain\r\n\r\nhello world";
    size = strlen(post);
    data = copy_of(post, size);
    CHECK_EQ(nw_http_parse_request(data, size, &req), 0);
    CHECK(strcmp(req.method.s, "POST") == 0);
    CHECK_EQ(req.query.len, 0);
    CHECK(strcmp(req.query.s, "") == 0);
    CHECK_EQ(req.body_size, 11);
    CHECK(memcmp(req.body, "hello world", 11) == 0);
    CHECK((char *)req.body + req.body_size == data + size);
    free(data);

    /* chunked, with an extension and a trailer: de-chunked in place */
    const char *chunked = "POST /c HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n1;ext=1\r\n \r\nA\r\nworld12345\r\n0\r\nX-Trailer: 1\r\n\r\n";
    size = strlen(chunked);
    data = copy_of(chunked, size);
    CHECK_EQ(nw_http_parse_request(data, size, &req), 0);
    CHECK_EQ(req.body_size, 16);
    CHECK(memcmp(req.body, "hello world12345", 16) == 0);
    free(data);
    EXPECT(0, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");

    /* connection handling by version */
    data = copy_of("GET / HTTP/1.1\r\nHost: h\r\nConnection: Keep-Alive, Close\r\n\r\n", 58);
    CHECK_EQ(nw_http_parse_request(data, 58, &req), 0);
    CHECK(!req.keep_alive);
    free(data);
    data = copy_of("GET / HTTP/1.0\r\n\r\n", 18);
    CHECK_EQ(nw_http_parse_request(data, 18, &req), 0);
    CHECK_EQ(req.version_minor, 0);
    CHECK(!req.keep_alive);                              /* 1.0 closes by default, needs no Host */
    CHECK_EQ(req.header_count, 0);
    free(data);
    data = copy_of("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 42);
    CHECK_EQ(nw_http_parse_request(data, 42, &req), 0);
    CHECK(req.keep_alive);
    free(data);

    EXPECT(0, "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT(0, "DELETE /x HTTP/1.1\r\nhost:h\r\n\r\n");
    EXPECT(0, "GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n");

    /* the request line */
    EXPECT(400, "GET / HTTP/1.1\r\n\r\n");                              /* 1.1 without Host */
    EXPECT(400, "GET  / HTTP/1.1\r\nHost: h\r\n\r\n");                  /* two spaces */
    EXPECT(400, "GET / HTTP/1.1 \r\nHost: h\r\n\r\n");
    EXPECT(400, "GET /a b HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT(400, "GET HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT(400, "GET /\r\nHost: h\r\n\r\n");                            /* HTTP/0.9 */
    EXPECT(400, "G@T / HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT(400, " GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT(400, "GET x HTTP/1.1\r\nHost: h\r\n\r\n");                   /* target must start with / */
    EXPECT(400, "GET http://h/x HTTP/1.1\r\nHost: h\r\n\r\n");          /* absolute form */
    EXPECT(400, "GET /%zz HTTP/1.1\r\nHost: h\r\n\r\n");                /* broken escape */
    EXPECT(400, "GET /a%2 HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT(400, "GET /a%00b HTTP/1.1\r\nHost: h\r\n\r\n");              /* NUL in the path */
    EXPECT(400, "GET /\x01 HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT(400, "GET / http/1.1\r\nHost: h\r\n\r\n");
    EXPECT(505, "GET / HTTP/2.0\r\nHost: h\r\n\r\n");
    EXPECT(400, "\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03\r\n\r\n");      /* a TLS hello */
    EXPECT(400, "\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n");

    /* header syntax */
    EXPECT(400, "GET / HTTP/1.1\r\nHost : h\r\n\r\n");                  /* space before the colon */
    EXPECT(400, "GET / HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n");        /* folded line */
    EXPECT(400, "GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n");
    EXPECT(400, "GET / HTTP/1.1\r\nHost: h\r\n: v\r\n\r\n");            /* empty name */
    EXPECT(400, "GET / HTTP/1.1\r\nHost: h\r\nX: a\rb\r\n\r\n");        /* bare CR */
    EXPECT(400, "GET / HTTP/1.1\r\nHost: h\r\nX: a\x01" "b\r\n\r\n");
    EXPECT(400, "GET / HTTP/1.1\nHost: h\n\n");                         /* bare LF line ends */
    EXPECT(400, "GET / HTTP/1.1\r\nHost: h\nX: 1\r\n\r\n");
    EXPECT(400, "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");        /* two Hosts */
    EXPECT(0,   "GET / HTTP/1.1\r\nHost: h\r\nX: tab\there\r\n\r\n");

    /* anything ambiguous about where the request ends is refused: this is
     * what request smuggling is made of */
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: +3\r\n\r\nabc");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3, 3\r\n\r\nabc");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 0x3\r\n\r\nabc");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length:\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999999999999999\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT(501, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
    EXPECT(501, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: xchunked\r\n\r\n0\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");     /* no chunking in 1.0 */
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nab");          /* body too short */
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabcd");        /* bytes left over */
    EXPECT(400, "GET / HTTP/1.1\r\nHost: h\r\n\r\nextra");
    EXPECT(400, "GET / HTTP/1.1\r\nHost: h\r\n");                                      /* head not finished */
    EXPECT(400, "");

    /* chunk syntax */
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n\r\n0\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcd\r\n0\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n3\nabc\r\n0\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n123456789\r\n\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n");
    EXPECT(400, "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nffffffff\r\n\r\n");

    /* exactly NW_HTTP_MAX_HEADERS headers fit, one more does not */
    char many[4096];
    int len = sprintf(many, "GET / HTTP/1.1\r\nHost: h\r\n");
    for (int i = 1; i < NW_HTTP_MAX_HEADERS; ++i)
        len += sprintf(many + len, "X-%d: %d\r\n", i, i);
    strcpy(many + len, "\r\n");
    data = copy_of(many, len + 2);
    CHECK_EQ(nw_http_parse_request(data, len + 2, &req), 0);
    CHECK_EQ(req.header_count, NW_HTTP_MAX_HEADERS);
    CHECK(strcmp(req.headers[NW_HTTP_MAX_HEADERS - 1].value.s, "63") == 0);
    free(data);
    strcpy(many + len, "X-Last: 1\r\n\r\n");
    EXPECT(431, many);

    /* url decoding on its own */
    char s1[] = "a%2Fb+c%7e";
    CHECK_EQ(nw_http_url_decode(s1, strlen(s1), false), 6);
    CHECK(strcmp(s1, "a/b+c~") == 0);
    char s2[] = "a+b";
    CHECK_EQ(nw_http_url_decode(s2, 3, true), 3);
    CHECK(strcmp(s2, "a b") == 0);
    char s3[] = "100%";
    CHECK_EQ(nw_http_url_decode(s3, 4, false), -1);
    char s4[] = "%4";
    CHECK_EQ(nw_http_url_decode(s4, 2, false), -1);
    char s5[] = "%41";
    CHECK_EQ(nw_http_url_decode(s5, 3, false), 1);
    CHECK(strcmp(s5, "A") == 0);
    CHECK_EQ(nw_http_url_decode(s5, 0, false), 0);

    /* response helpers */
    nw_http_response rsp;
    memset(&rsp, 0, sizeof(rsp));
    for (int i = 0; i < NW_HTTP_MAX_HEADERS; ++i)
        CHECK_EQ(nw_http_add_header(&rsp, "X", "y"), 0);
    CHECK_EQ(nw_http_add_header(&rsp, "X", "y"), -1);
    CHECK(strcmp(nw_http_status_text(404), "Not Found") == 0);
    CHECK(strcmp(nw_http_status_text(799), "Unknown") == 0);

    /*
     * robustness. every prefix of valid requests, then a few hundred
     * thousand mutated and random inputs. nothing is asserted about the
     * verdict: the point is that AddressSanitizer stays silent, and that a
     * request that is accepted only points inside its own buffer.
     */
    const char *seeds[] = { get, post, chunked,
        "GET /x?y HTTP/1.0\r\nConnection: keep-alive\r\nA: b\r\n\r\n",
        "PUT /p HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\nbody",
    };
    int accepted = 0, refused = 0;
    for (size_t k = 0; k < sizeof(seeds) / sizeof(seeds[0]); ++k) {
        size_t full = strlen(seeds[k]);
        for (size_t cut = 0; cut < full; ++cut) {
            data = copy_of(seeds[k], cut);
            CHECK(nw_http_parse_request(data, cut, &req) != 0);
            free(data);
        }
    }
    static const char interesting[] = "\r\n: ;%0123456789abcdefx \t\0,/?*";
    for (int iter = 0; iter < 300000; ++iter) {
        const char *seed = seeds[rnd() % (sizeof(seeds) / sizeof(seeds[0]))];
        size = strlen(seed);
        if (rnd() % 8 == 0)
            size = rnd() % (size + 1);
        data = copy_of(seed, size);
        int changes = rnd() % 4;
        for (int c = 0; c < changes && size > 0; ++c) {
            size_t at = rnd() % size;
            switch (rnd() % 4) {
            case 0: data[at] = (char)rnd(); break;
            case 1: data[at] = interesting[rnd() % (sizeof(interesting) - 1)]; break;
            case 2: memmove(data + at, data + at + 1, size - at - 1); size--; break;
            default:
                if (at + 1 < size)
                    data[at + 1] = data[at];
                break;
            }
        }
        if (nw_http_parse_request(data, size, &req) == 0) {
            accepted++;
            char *end = data + size;
            CHECK(req.method.s >= data && req.method.s + req.method.len < end);
            CHECK(req.path.s >= data && req.path.s + req.path.len < end);
            CHECK(req.query.s >= data && req.query.s + req.query.len < end);
            CHECK(req.method.s[req.method.len] == '\0' && req.path.s[req.path.len] == '\0');
            CHECK(req.query.s[req.query.len] == '\0');
            CHECK(req.header_count <= NW_HTTP_MAX_HEADERS);
            for (uint32_t i = 0; i < req.header_count; ++i) {
                nw_http_header *h = &req.headers[i];
                CHECK(h->name.s >= data && h->value.s >= h->name.s && h->value.s + h->value.len < end);
                CHECK(strlen(h->name.s) == h->name.len && strlen(h->value.s) == h->value.len);
            }
            CHECK((char *)req.body >= data && (char *)req.body + req.body_size <= end);
            volatile char sum = 0;
            for (size_t i = 0; i < req.body_size; ++i)
                sum += ((char *)req.body)[i];
        } else {
            refused++;
        }
        free(data);
    }
    fprintf(stderr, "mutated inputs: %d accepted, %d refused\n", accepted, refused);
    CHECK(accepted > 1000 && refused > 1000);

    return 0;
}
