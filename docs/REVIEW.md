# asnw 代码 Review（基于 63de198）

范围：`asnw/` 全部 9 个模块（约 2300 行）、`wrapper/ut_rpc`、`test/` 与 `c1000k/` 示例。
方法：通读源码；`gcc -Wall -Wextra`、cppcheck；为每个模块写单测，并在 ASan+UBSan、TSan、valgrind 下运行。

标注说明：**已复现** = 有对应的 `unittest/kb_*.c`，`make check-asan` 可重现；**推断** = 由代码路径推出，尚未写测试验证。

## 总体评价

分层清楚（buf → sock → ses → svr/clt → wrapper），接口小而一致，`-Wall` 零警告。主路径是可靠的：
tcp / udp / unix stream / unix dgram / seqpacket 五种传输的回显、fd 传递、断线重连、线程池、状态机、RPC 封装，
在 ASan+UBSan 与 valgrind 下全部通过（stream 用例单连接往返约 2MB，覆盖了写缓冲链表），线程池在 TSan 下无数据竞争。

问题集中在三类路径：**释放/关闭**、**错误处理**、**数据报**。这些路径在示例程序里基本走不到，所以长期没有暴露。

## 高优先级（已复现）

| # | 问题 | 位置 | 复现 |
|---|------|------|------|
| H1 | `nw_svr_release` 对 `clt_cache` 释放两次：先 `nw_cache_release`，随后 `nw_svr_free` 再释放一次。任何一次正常关停都会触发。同一路径上监听 ses 的 `host_addr` 还会泄漏（第一次 release 后 `write_buf` 为 NULL，`nw_svr_free` 里的 `free(host_addr)` 被跳过）。 | nw_svr.c:313, :82 | kb_svr_release_double_free |
| H2 | `on_can_write` 里 `int nwrite` 与 `size_t size` 比较。`sendmsg` 返回 -1 时被转成 SIZE_MAX，错误分支被跳过，包被当作已发送从队列移除。结果：seqpacket 积压队列遇到 EAGAIN 就**静默丢包**，真实错误也被吞掉。实测发 20 个 100KB 包，每次 `nw_ses_send` 都返回 0、无任何错误回调，对端只收到 4 个。 | nw_ses.c:266 | kb_seqpacket_drop |
| H3 | 向已关闭的对端写数据会触发 SIGPIPE。库内用 `write`/`sendmsg` 且不带 `MSG_NOSIGNAL`，也没有忽略 SIGPIPE；应用若忘了 `signal(SIGPIPE, SIG_IGN)`，一个客户端提前断开就能杀掉整个服务进程。 | nw_ses.c:57, :85 | kb_sigpipe |
| H4 | `connect` 同步失败（unix 路径不存在、backlog 满等）时失败路径执行两遍：`nw_ses_connect` 先调 `on_connect(false)` 走完一遍，返回 -1 后 `nw_clt_start` 再走一遍。后果：用户 `on_close` 每次被调两次；`ut_rpc_clt` 在 `on_close` 里轮换地址，会每次跳过一个；若 `on_close` 返回 >0（立即重连），两条路径互相递归直到**栈溢出**。 | nw_clt.c:244-255, nw_ses.c:392 | kb_clt_connect_fail_twice（带任意参数运行可见栈溢出） |
| H5 | `nw_clt_release` 不停 `clt->timer`。连接超时定时器在连接成功后仍会保持 `reconnect_timeout` 秒，此窗口内（或重连等待期间）释放 client，libev 之后会在已释放内存上回调。 | nw_clt.c:276 | kb_clt_release_timer_uaf |
| H6a | 数据报 client 的 `recvfrom` 直接写 `ses->peer_addr`，覆盖了配置的服务端地址。任何第三方发来一个 UDP 包，client 之后的数据就全部发往该第三方。对 UDP 来说源地址可伪造，属于安全问题。 | nw_ses.c:163 | kb_udp_clt_peer_hijack |
| H6b | 同一行：`peer_addr.addrlen` 作为 value-result 参数传入后从不复位。unix dgram 服务端收到短路径来源后，更长的来源地址被截断，回包失败或发给上一个发送方。`nw_clt` 自动生成的路径等长，所以用自家 client 测不出来。 | nw_ses.c:163 | kb_unix_dgram_addrlen |
| H7 | （wrapper）`rpc_decode` 的长度用 uint32 相加：`body_size` 接近 4G 时回绕成很小的数，通过 `RPC_MAX_PKG_SIZE` 检查；crc 由发送方自己算，也能通过。业务回调拿到 `body_size=0xFFFFFFxx` 和一个指向 1MB 缓冲区的指针，未认证的对端即可造成越界读。 | ut_rpc.c:40 | kb_rpc_decode_overflow |
| H8 | fd 耗尽时 accept 循环空转：`EMFILE` 只上报错误就返回，连接仍在 backlog、监听 fd 持续可读，libev 立刻再次回调。实测一个挂起连接 0.2 秒内产生约 17 万次错误回调，CPU 100%。对面向 C1000K 的库，这是最容易在生产遇到的一个。 | nw_ses.c:300-312 | kb_accept_emfile_spin |

## 中优先级

| # | 问题 | 状态 |
|---|------|------|
| M1 | `nw_sock_cfg_parse` 实际上解析不了任何 IPv6 地址：host/port 用第一个 `:` 切分（nw_sock.c:196），`tcp@::1:8080` 和 `tcp@[::1]:8080` 都返回 -4。头文件注释写的是支持 IPv6；底层 `nw_addr_t`/ses 是支持的，只差解析。另外端口未校验：非数字得到 0，超过 65535 被截断。 | 已复现 kb_ipv6_cfg_parse |
| M2 | `nw_job_release` 不释放 `job->cache`（泄漏）；release 时仍在排队的请求、未投递的结果被直接丢弃，不调用 `on_cleanup`，request/reply 内存由谁释放没有约定。 | 已复现 kb_job_release_leak |
| M3 | `nw_ses_bind` 对 unix 地址无条件 `unlink(path)` 再 `chmod 0777`（nw_ses.c:361-367）：配置写错路径会删掉普通文件；socket 对所有本机用户可写且无法配置。unix dgram client 的本地路径用 `srand(时间)`+`rand()` 生成在 /tmp 下，可预测、会重置应用自己的随机数种子、关闭后不删除。 | 代码确认 |
| M4 | `set_socket_option` 失败时会 `close(sockfd)`，但三个调用点都忽略返回值（nw_svr.c:98, :262，nw_clt.c:111, :262），之后继续把已关闭的 fd 注册进 libev；fd 号被复用后会串连接。`setsockopt(SO_RCVBUF)` 很少失败，所以概率低、后果重。 | 推断 |
| M5 | `nw_ses` 对象经 `nw_cache` 复用，连接关闭后同一指针可能已是另一个连接。业务若跨异步（nw_job / nw_state）持有 `nw_ses *`，回包会发给别人。`ses->id` 可以用来校验，但头文件没有说明这一约定。`on_can_read` 自身也依赖这一点：回调后用 `if (!ses->read_buf)` 判断 ses 是否已被关闭，读的是已归还 cache 的内存；`on_recv_fd` 之后漏了这个判断（nw_ses.c:229）。 | 推断 |
| M6 | dgram/seqpacket 路径下 `decode_pkg` 返回 0 时仍然调用 `on_recv_pkg(…, 0)`（nw_ses.c:185, :240）。`ut_rpc` 的回调不看 size，会去解析缓冲区里上一个包的残留头部。 | 推断 |
| M7 | 没有背压与上限：`buf_limit=0` 表示写队列无限（慢读对端可以耗尽内存，示例都用 0）；`nw_ses_send` 返回 0 时调用方无法区分“已发出”和“已排队”；dgram 发送遇到 EAGAIN 直接报错不排队（unix dgram 默认队列只有 10 个包，很容易触发）；stream 读循环读到 EAGAIN 为止，没有单次事件上限，一个高速连接可以饿死其他连接；nw_job 请求队列无上限。 | 设计取舍，建议写进文档 |
| M8 | 示例与构建已经失修：`test/`、`wrapper/`、`c1000k/` 三个 makefile 仍指向改名前的 `../network`，全部编不过；`test_state.c` 用的是旧的两参数 `nw_state_add`；README 里的 `listener_worker` 启动即失败（inner svr/clt 没设 `decode_pkg`，`nw_*_create` 返回 NULL）；`test_clt.c`、`listener_worker.c` 的 `on_close` 声明返回 int 却没有 return（返回值决定是否立即重连，属未定义行为）；`echo_server.c:83` 的 `memset` 参数写反。 | 已复现；makefile 与 test_state.c 已在本分支修正，其余未动 |

## 低优先级

- **L1 OOM 路径**：`nw_clt_create` 的 `malloc` 未判空；`buf_pool` 创建失败时走 `nw_clt_release` → `nw_ses_close`，此时 `sockfd` 是 memset 出来的 0，会 `close(0)`；`host_addr` 在 `nw_ses_init` 失败时泄漏（cppcheck 报出）；`nw_svr_create` 在 pool 创建失败时 `nw_svr_free` 会遍历尚未清零的 `svr_list`。
- **L2** `nw_evt.c:22`：`readlink` 失败返回 -1 时写 `exe_path[-1]`。
- **L3** `nw_ses_send_fd` 的 control 缓冲区未清零，cmsg 填充字节未初始化（valgrind 报出，已在 `unittest/valgrind.supp` 抑制并注明）。
- **L4** fd 都没有 `CLOEXEC`，accept 后再单独 `fcntl`（可换 `accept4(SOCK_NONBLOCK|SOCK_CLOEXEC)`，每个连接省两次系统调用）；`F_GETFL` 返回值未检查。
- **L5** `nw_timer_set` 作用于活动中的定时器是 libev 的未定义行为，接口上没有防护；`nw_loop_break_later` 在 loop 运行前调用会被静默忽略。
- **L6** `nw_state`：`on_timeout` 回调里若删除自己并用同一 id 重新添加，回调返回后新条目会被连同活动定时器一起移除；迭代期间添加条目可能触发 rehash 使迭代器失效。
- **L7** wrapper：心跳用 `gettimeofday`，系统调时会造成批量断连（应使用 `ev_now` 或 CLOCK_MONOTONIC）；`rpc_clt_release` 泄漏 `name` 与 `addr_arr`；`rpc_send` 使用 1MB 静态缓冲区，非线程安全。
- **L8** `asnw/makefile` 没有头文件依赖（`clean` 里删 `*.d` 但从不生成），改了结构体后旧 .o 不会重编；`-Wno-uninitialized` 关掉了一类有用告警；加 `-Wextra` 本可直接发现 H2。

## 建议的修复顺序

1. H2、H1、H5、H3：都是几行的改动，风险小，收益最大。
2. H4：把“连接失败”的处理收敛到一处（`nw_ses_connect` 只返回错误，不回调），顺带解决递归。
3. H6：`recvfrom` 前复位 `addrlen`；client 端收包用临时地址，或对 dgram client 直接 `connect()`。
4. H8：EMFILE/ENFILE 时暂停 accept 一小段时间（或预留一个备用 fd）；`ECONNABORTED` 应继续循环。
5. H7 与 M6：`rpc_decode` 先分别校验 `ext_size`/`body_size` 再相加；数据报路径 decode 返回 0 时丢弃。
6. 之后再处理 M1–M5。M5、M7 主要是把约定写进头文件。

每修一个，把对应的 `unittest/kb_xxx.c` 改名为 `t_xxx.c`，它就成为回归测试；`run.sh` 会把已修复但未改名的用例报成 XPASS 提醒。
