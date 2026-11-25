# asnw

A small asynchronous network library for Linux, written in C on top of [libev](http://software.schmorp.de/pkg/libev.html).

You describe a server or a client with a config struct and a handful of callbacks; asnw does the sockets, buffering, message framing, reconnecting and timers. One API covers five transports: TCP, UDP, and unix stream / datagram / seqpacket sockets (including file descriptor passing).

About 3,000 lines, no dependency other than libev, single threaded by design.

## Modules

| Module | What it is |
|--------|------------|
| `nw_svr` | Server. One instance can listen on several addresses of different types at once. |
| `nw_clt` | Client. Keeps a connection to one address and reconnects automatically. |
| `nw_timer` | Call a function after an interval, once or repeatedly. |
| `nw_state` | Table of pending states that expire, for request tracking with timeouts. |
| `nw_job` | Thread pool. Work runs on worker threads, results come back on the loop thread. |
| `nw_buf` | Buffer pool, buffer list and a fixed size object cache. |
| `nw_sock` | Address parsing and socket option helpers. |
| `nw_evt` | The event loop (`nw_loop_run`, `nw_loop_break`). |
| `nw_ses` | A session: one socket and its buffers. Used by `nw_svr` and `nw_clt`; you receive it in callbacks. |

## Build

```
sudo apt-get install build-essential libev-dev
make            # library, wrapper, examples and tests into build/debug
make check      # run the tests
```

The library is `build/debug/libasnw.a` (`make VARIANT=release` for an optimized one in `build/release`). To use it, add `asnw/` to the include path and link with `-lasnw -lev -lpthread`.

`asnw/makefile` also still builds `asnw/libasnw.a` on its own.

## A server in 40 lines

A line based echo server listening on TCP and on a unix socket:

```c
# include <stdio.h>
# include <string.h>
# include "nw_svr.h"

/* where does a message end? return its size, 0 if incomplete, < 0 if broken */
static int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    char *end = memchr(data, '\n', max);
    return end ? (int)(end - (char *)data) + 1 : 0;
}

/* called once per complete message */
static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_ses_send(ses, data, size);
}

int main(void)
{
    nw_svr_bind bind[2];
    if (nw_sock_cfg_parse("tcp@127.0.0.1:1234", &bind[0].addr, &bind[0].sock_type) < 0 ||
        nw_sock_cfg_parse("stream@/tmp/echo.sock", &bind[1].addr, &bind[1].sock_type) < 0)
        return 1;

    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 2;
    cfg.bind_arr = bind;
    cfg.max_pkg_size = 10240;

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = decode_pkg;
    type.on_recv_pkg = on_recv_pkg;

    nw_svr *svr = nw_svr_create(&cfg, &type, NULL);
    if (svr == NULL || nw_svr_start(svr) < 0)
        return 1;

    nw_loop_run();
    return 0;
}
```

`decode_pkg` and `on_recv_pkg` are required. The optional callbacks are `on_new_connection`, `on_connection_close`, `on_error_msg`, `on_accept`, `on_recv_fd` and `on_privdata_alloc` / `on_privdata_free` for per connection data. `nw_svr.h` documents each one.

A client is built the same way from `nw_clt_cfg` and `nw_clt_type`, then `nw_clt_start`. See `test/test_clt.c`.

## Addresses

Addresses are written `protocol@address` and parsed with `nw_sock_cfg_parse`:

| Example | Transport |
|---------|-----------|
| `tcp@127.0.0.1:3333` | TCP |
| `tcp@[::1]:3333` | TCP over IPv6 |
| `udp@127.0.0.1:3333` | UDP |
| `stream@/tmp/a.sock` | unix stream |
| `dgram@/tmp/a.sock` | unix datagram |
| `seqpacket@/tmp/a.sock` | unix seqpacket |

The protocol name is case insensitive. Hosts are numeric addresses; there is no name resolution.

## Things to know

**Messages.** On stream connections `decode_pkg` finds message boundaries, and a message can be at most `max_pkg_size` bytes. On datagram and seqpacket sockets every packet is one message; a packet for which `decode_pkg` returns 0 is dropped.

**Session lifetime.** A `nw_ses *` is valid until its connection closes, and the memory is reused for later connections. If you keep one across asynchronous work (a job, a state, a timer), save `ses->id` with it and check that it still matches before using the pointer. Ids are never 0, and a closed session has id 0.

**Send buffering.** `nw_ses_send` writes directly when it can and queues the rest. `buf_limit` caps that queue in units of `max_pkg_size` buffers; the default 0 means unlimited, so set a limit on servers that face untrusted clients. Datagram sends are never queued: a full socket buffer is reported as an error.

**Datagram servers.** The session passed to `on_recv_pkg` is the listening one, and `nw_ses_send` replies to the sender of the datagram being handled. Reply inside the callback, or copy `ses->peer_addr`.

**Clients.** A stream client retries every `reconnect_timeout` seconds (default 1.0), which is also its connect timeout. Return > 0 from `on_close` to retry at once. Check `nw_clt_connected` before sending. A datagram client is usable right after `nw_clt_start` and ignores UDP packets from any address other than its server.

**Threads.** Everything runs on the thread that calls `nw_loop_run`, and the library is not thread safe. Use `nw_job` to move blocking work to worker threads; its `on_finish` callback runs back on the loop thread.

**Unix sockets.** Binding replaces a stale socket file at the path and makes the socket accessible to all local users (mode 0777).

## Examples

Built into `build/debug/test/`, `build/debug/wrapper/` and `build/debug/c1000k/`.

| Program | What it shows |
|---------|---------------|
| `test/echo_server` | Echo server on any number of addresses: `echo_server 'tcp@127.0.0.1:1234' 'stream@/tmp/echo.sock'` |
| `test/test_clt` | Client that pings a server once a second: `test_clt 'tcp@127.0.0.1:1234'` |
| `test/listener_worker` | One process accepts and hands each connection to one of N worker processes by passing its fd: `listener_worker 'tcp@127.0.0.1:1234' 10` |
| `test/test_timer`, `test/test_state` | Timer and state table |
| `test/test_*_clt` | Plain socket clients, one per transport, for poking at a server |
| `c1000k/` | Server and client for a one million connection test |
| `wrapper/` | `ut_rpc`, an RPC layer built on `nw_svr` and `nw_clt`: a common header with CRC, request / reply / push packets, heartbeats and failover across several server addresses |

## Testing

```
make check            # unit tests
make check-asan       # under AddressSanitizer and UBSan
make check-tsan       # thread pool under ThreadSanitizer
make check-valgrind   # under valgrind
make lint             # gcc -Wextra and cppcheck
```

The tests live in `unittest/`, one executable per test. See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for the conventions and [docs/REVIEW.md](docs/REVIEW.md) for the code review notes (in Chinese).

## License

MIT, see [LICENSE](LICENSE).
