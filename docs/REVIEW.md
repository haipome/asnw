# asnw 代码 Review（基于 63de198）

> **状态更新**：下列问题已在 `dev-setup` 分支修复，逐项状态见文末「修复状态」。正文保留为修复前的描述，
> 其中提到的 `kb_*.c` 复现用例已改名为 `t_*.c`，成为回归测试。

范围：`asnw/` 全部 9 个模块（约 2300 行）、`wrapper/ut_rpc`、`test/` 与 `c1000k/` 示例。
方法：通读源码；`gcc -Wall -Wextra`、cppcheck；为每个模块写单测，并在 ASan+UBSan、TSan、valgrind 下运行。

标注说明：**已复现** = 有对应的复现用例；**推断** = 由代码路径推出。

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

## 修复状态

| # | 状态 | 做法 | 回归测试 |
|---|------|------|----------|
| H1 | 已修复 | `nw_svr_free` 成为唯一的释放点：先释放各 ses 与 `host_addr`，再释放 cache，最后释放 buf pool | t_svr_release |
| H2 | 已修复 | stream 与 packet 两种写法分开判断，不再做有符号/无符号混合比较；EAGAIN 时包留在队列里 | t_seqpacket_queue |
| H3 | 已修复 | 所有发送改用 `send`/`sendmsg`/`sendto` 加 `MSG_NOSIGNAL` | t_sigpipe |
| H4 | 已修复 | `nw_ses_connect` 同步失败只返回 -1、不再回调；失败处理收敛到 `close_and_reconnect` 一处；“立即重连”改为经事件循环的 0 延时定时器，不再递归 | t_clt_connect_fail |
| H5 | 已修复 | `nw_clt_release` 先停定时器；连接成功后即解除连接超时定时器 | t_clt_release |
| H6a | 已修复 | client 的 `peer_addr` 不再被来源地址覆盖；UDP client 丢弃非配置地址发来的包（目标是广播/多播/通配地址时除外） | t_udp_clt_peer |
| H6b | 已修复 | 每次 `recvfrom` 使用独立的地址缓冲并复位长度 | t_unix_dgram_addrlen |
| H7 | 已修复 | 先单独校验 `body_size`，再求和 | t_rpc_decode |
| H8 | 已修复 | 预留一个备用 fd：EMFILE/ENFILE 时用它接收并立即关闭该连接，每个被拒连接上报一次错误；`ECONNABORTED` 继续循环 | t_accept_emfile |
| M1 | 已修复 | 支持 `[ipv6]:port`（也接受 `ipv6:port`，端口取最后一个冒号之后）；端口必须是 0–65535 的十进制数；IPv6 地址打印为 `[ip]:port` | t_ipv6、t_sock |
| M2 | 已修复 | release 时释放 cache；未执行/未投递的任务逐个调用 `on_cleanup` | t_job_release |
| M3 | 部分修复 | bind 前只删除已存在的 **socket** 文件；dgram client 的本地路径不再动 `rand()` 种子，关闭时删除。**`chmod 0777` 未改**（见下） | t_svr_release、t_echo |
| M4 | 已修复 | 设置缓冲区大小失败不再关闭 fd（尽力而为） | — |
| M5 | 已修复 | `on_recv_fd` 后补上关闭检查；读写事件回调在 ses 被关闭或重启后不再继续写；生命周期约定写入 `nw_svr.h` | t_seqpacket_fd |
| M6 | 已修复 | dgram/seqpacket 下 decode 返回 0 的包直接丢弃 | t_unix_dgram_addrlen |
| M7 | 仅文档 | `buf_limit=0` 的含义、dgram 不排队、job 队列无上限已写入头文件。**行为未改**（见下） | — |
| M8 | 已修复 | makefile、`test_state.c`、`listener_worker`（补 `decode_pkg`、失败时关 fd）、两处 `on_close` 返回值、两处 `memset` 参数 | 手工冒烟：echo_server、listener_worker |
| L1 | 已修复 | `malloc` 判空；`nw_ses_init` 把 `sockfd` 置 -1；`svr_list` 用 `calloc` | t_clt_release |
| L2 | 已修复 | `readlink` 失败时使用默认名 | — |
| L3 | 已修复 | control 缓冲区清零，valgrind 抑制文件已删除 | t_seqpacket_fd（valgrind） |
| L4 | 部分修复 | `F_GETFL` 返回值已检查。**`CLOEXEC`/`accept4` 未改**（见下） | — |
| L5 | 仅文档 | 约束写入 `nw_timer.h`、`nw_evt.h`；库内部重设定时器前都先 stop | — |
| L6 | 已修复 | 超时回调前先把条目摘出哈希表，回调里对同一 id 的任何操作都安全 | t_state |
| L7 | 部分修复 | 心跳改用单调时钟（新增 `rpc_monotonic_time`，`current_timestamp` 语义不变）；`rpc_clt_release` 不再泄漏。**`rpc_send` 的静态缓冲区未改** | t_rpc |
| L8 | 已修复 | `asnw/makefile` 增加 `-MMD -MP` 与 `-Wextra`，去掉 `-Wno-uninitialized` 和无效的 `-rdynamic` | — |

### 有意没改的

这几项会改变对外行为或属于设计取舍，需要你来定：

- **unix socket 的 `chmod 0777`**：收紧权限可能让以其他用户运行的客户端连不上。建议在 `nw_svr_bind` 里加一个可选的 mode 字段，默认保持 0777。
- **`CLOEXEC` / `accept4`**：对 fork 后不 exec 的用法没有影响，但会改变 fd 继承语义；收益是每个连接省两次系统调用。
- **背压与上限（M7）**：`buf_limit` 的默认值、`nw_ses_send` 是否返回“已排队”、stream 单次事件的读取上限、job 队列上限，都是接口层面的决定。
- **`rpc_send` 的 1MB 静态缓冲区**：单线程使用没有问题，改动需要调整 wrapper 的内存模型。

### 行为变化（升级时需要知道）

1. client 的 `on_close` 现在每次失败只调用一次；`on_close` 返回 >0 的立即重连改为下一轮事件循环执行。
2. `nw_ses_connect` 同步失败不再调用 `on_connect(false)`（nw_ses 是内部接口，`nw_clt` 的行为不变：用户仍会收到一次 `on_connect(false)`）。
3. 对已启动的 client 再次调用 `nw_clt_start` 会先关闭旧连接再重连（原来会泄漏旧 fd）。
4. UDP client 丢弃来源不是配置地址的数据报。
5. dgram/seqpacket 下 `decode_pkg` 返回 0 的包不再以 size=0 交给 `on_recv_pkg`。
6. 端口只接受十进制（原来 `strtoul` 以 0 为基数，`0x50` 这类写法也能通过）；IPv6 地址打印格式变为 `[ip]:port`。
7. bind unix 地址时，如果路径上是普通文件，不再删除它，bind 会失败。
8. `nw_state` 的 `on_timeout` 回调期间，该 state 已不在表中（`nw_state_get` 返回 NULL）。
9. `nw_job_release` 会为未完成的任务调用 `on_cleanup`。
10. fd 耗尽时新连接会被接收后立即关闭，而不是留在 backlog 里等待。

### 验证范围

21 个用例在 debug、release、ASan+UBSan、TSan、valgrind 五种构建下全部通过。

补充（整理头文件注释时发现并修复）：`rpc_clt` 的 `curr_index` 未初始化，首次连接就失败时会用随机下标读 `addr_arr`；回归测试 t_rpc_failover。
**IPv6 只验证了地址解析**：验证环境没有 IPv6 回环地址，`t_echo` 里的 IPv6 回显用例被自动跳过，需要在有 IPv6 的机器上跑一次 `make check`。
