# 开发与测试

依赖（Ubuntu/Debian）：

    sudo apt-get install build-essential libev-dev valgrind cppcheck curl
    pip install websockets        # 可选，WebSocket 互通测试用

常用命令（在仓库根目录）：

    make                  # 库 + wrapper + 示例 + 单测，输出到 build/debug
    make check            # 运行单测
    make check-asan       # ASan + UBSan 下运行，并运行已知缺陷复现用例 kb_*
    make check-tsan       # TSan 下运行全部单测
    make check-valgrind   # valgrind memcheck
    make coverage         # 覆盖率构建下跑测试，按文件打印行覆盖率
    make lint             # gcc -Wextra + cppcheck
    make check-asan T=t_echo      # 只跑一个
    make VARIANT=release          # -O2 构建到 build/release

各子目录原有的 makefile 仍可单独使用（已改为指向 `../asnw`）。

## 测试约定

- 一个测试一个可执行文件，退出码 0 即通过；公共工具在 `unittest/common.h`。
- `t_*.c` 必须通过。
- 发现新缺陷时先写最小复现，命名为 `kb_*.c`：它预期失败（报告为 XFAIL），只在 asan 变体下运行。
  修复后改名为 `t_*.c` 成为回归测试；忘了改名，`run.sh` 会报 XPASS 并让本次运行失败。目前没有未修复的 `kb_*`。
- 日志在 `build/<variant>/unittest/log/<name>.log`。
- 测试只使用回环地址的临时端口和 `/tmp/asnw-ut-XXXXXX/` 下的 unix socket，可在容器里运行。
- 主机没有 IPv6 回环地址时，`t_echo` 的 IPv6 用例自动跳过（日志里有 `skipped`）。

## 现有用例

| 用例 | 覆盖 |
|------|------|
| t_buf | nw_buf / pool / list / cache |
| t_dict | 增删改查、扩容、不同分布的 key、遍历中删除 |
| t_sock、t_ipv6 | 地址解析（IPv4、IPv6、unix、非法输入）与 socket 选项 |
| t_timer | 单次、重复、停止 |
| t_state | 增删改查、超时、扩容、迭代、超时回调中用同一 id 重新添加 |
| t_job、t_job_release | 线程池往返；带着未完成任务 release |
| t_echo | tcp、tcp/ipv6、udp、unix stream/dgram/seqpacket 回显，随后 release；stream 约 2MB |
| t_seqpacket_fd、t_seqpacket_queue | fd 传递；写队列积压时不丢包 |
| t_clt_reconnect、t_clt_connect_fail、t_clt_release | 重试与重连；同步失败只处理一次、立即重连不递归；各时点 release |
| t_clt_close_callback | `on_close` 里发送不递归、`on_close` 里调 `nw_clt_close` 表示不再重连、连接迟迟无应答时超时重试 |
| t_svr_get_clt | 按 id 找回连接；连接关闭、内存被新连接复用后旧 id 查不到 |
| t_svr_timeout | 空闲超时（全局与单连接）、截止时间、on_timeout 回调、有序关闭时最后的数据不丢 |
| t_svr_close_callback | 关闭回调里发送、重复关闭、关闭其他连接（连锁）都只触发一次回调；遍历时任意连接被关闭都安全 |
| t_hub | `nw_hub`：连接轮流分给各线程、回调在所属线程执行、按 id 跨线程发送/关闭/广播、`nw_hub_post`、队列上限、启动失败回滚、释放过程中仍有跨线程发送 |
| t_http_hub | `nw_http` 多线程模式：在所属线程上直接回复、经每线程自己的 `nw_job` 稍后回复、主线程按 id 跨线程回复（含自定义头、大 body、回复后关闭）、`100 Continue`、单 worker、启动失败 |
| t_ws_hub | `nw_ws` 多线程模式（`thread_count`）：按 id 发送和关闭、广播（含未握手连接收不到）、各线程同时广播时单个连接的消息顺序、单 worker、启动失败 |
| t_threads | 4 个线程各自一个 loop，同时跑 server、client、timer、state、job、rpc，互不干扰（要在 TSan 下跑才有意义） |
| t_flow | 流量控制：`nw_ses_pending`、`on_drain`、暂停/恢复读取（含已读入缓冲的消息、seqpacket、暂停期间对端断开、暂停后 shutdown）；中继场景下不做流控会被断开、做了则积压不超过设定值 |
| t_read_once | 流式读取每个事件只读一次（读不满就不再读）；数据超过或恰好填满缓冲区、数据和断开一起到达、关闭中的连接 |
| t_cloexec | 库创建和接受的所有 socket 都是非阻塞加 close-on-exec，`popen` 出来的子进程里看不到它们 |
| t_svr_release | 空闲、带活动连接、多地址 server 的 release；bind 不删除普通文件 |
| t_accept_emfile | fd 耗尽时拒绝连接而不空转，恢复后正常 |
| t_sigpipe | 对端已关闭时发送不触发 SIGPIPE |
| t_udp_clt_peer、t_unix_dgram_addrlen | 数据报 client 不被第三方改写目标；server 回包地址正确；残包丢弃 |
| t_http_parse | 请求解析：原地切分、各类非法请求对应的状态码、截断与三十万次变异输入下不越界 |
| t_http_svr | HTTP 服务端：keep-alive、分段到达、chunked、HEAD、100-continue、按 id 异步回包、拒绝与超时、连接的各种结束方式 |
| t_http_curl | 用 curl 做互通测试；没装 curl 时自动跳过 |
| t_ws_svr | WebSocket 服务端：握手与各种拒绝、各长度档的消息、分片、控制帧、协议违规、双向关闭、广播、按 id 推送、ping 与空闲超时、变异帧 |
| t_ws_interop | 用 Python websockets 库做互通测试；没装时自动跳过 |
| t_rpc、t_rpc_decode、t_rpc_failover | ut_rpc 请求/应答、心跳保活；长度与 crc 校验；首个地址不可用时切换到下一个 |
| t_crc32 | ut_crc32：一次 8 字节的查表实现与逐位计算的标准 CRC-32 一致，覆盖各种长度、各种对齐和 1M 的包 |

## 覆盖率与已知的测试缺口

`make coverage` 的结果目前整体在 85% 以上。没有覆盖到的主要是：

- 各处内存分配失败的分支（没有做故障注入）。
- WebSocket 的握手超时（固定 10 秒，测试要等这么久；它用的截止时间机制本身在 `t_svr_timeout` 里测过）。
- `nw_ws.c` 里 SHA-1 的多块路径：握手的输入固定 60 字节，走不到。这份 SHA-1 只为握手服务，不要当通用实现对外使用。
- `nw_evt.c` 的致命错误处理和 `nw_loop_break_later`。
- IPv6 的端到端路径在没有 IPv6 回环地址的机器上会被跳过。
- WebSocket 没有跑过 Autobahn 测试套件（需要 Docker）。

## 写回调时的几条约定

这些在头文件里都有说明，集中列一下：

- 每个线程一个 loop，对象属于创建它的线程：回调在该线程的 `nw_loop_run` 里执行，也只有该线程可以对它调用函数。库里没有锁。只有 `nw_job` 的 `on_job` 在工作线程上。
- 库里新增全局或静态变量时，要么 `__thread`，要么只读，要么加锁（目前唯一加锁的是 EMFILE 备用 fd），并让 `t_threads` 覆盖到。
- `decode_pkg` 里不能发送、不能关闭连接。
- 传给回调的数据（消息、HTTP 请求、WebSocket 消息）只在回调期间有效。
- 跨异步保存 `ses->id` 而不是 `nw_ses *`，回来时用 `nw_svr_get_clt` 取。
- 关闭回调（`on_connection_close`、`on_close`）被调用时，这个连接已经从 server 里摘掉：对它发送返回 -1，再次关闭无效；对其他连接发送、关闭都可以。
- 不要在某个 server 或 client 自己的回调里 release 它本身。
