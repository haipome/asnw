# 开发与测试

依赖（Ubuntu/Debian）：

    sudo apt-get install build-essential libev-dev valgrind cppcheck

常用命令（在仓库根目录）：

    make                  # 库 + wrapper + 示例 + 单测，输出到 build/debug
    make check            # 运行单测
    make check-asan       # ASan + UBSan 下运行，并运行已知缺陷复现用例 kb_*
    make check-tsan       # TSan 下运行线程池测试
    make check-valgrind   # valgrind memcheck
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
| t_svr_get_clt | 按 id 找回连接；连接关闭、内存被新连接复用后旧 id 查不到 |
| t_svr_release | 空闲、带活动连接、多地址 server 的 release；bind 不删除普通文件 |
| t_accept_emfile | fd 耗尽时拒绝连接而不空转，恢复后正常 |
| t_sigpipe | 对端已关闭时发送不触发 SIGPIPE |
| t_udp_clt_peer、t_unix_dgram_addrlen | 数据报 client 不被第三方改写目标；server 回包地址正确；残包丢弃 |
| t_rpc、t_rpc_decode、t_rpc_failover | ut_rpc 请求/应答、心跳保活；长度与 crc 校验；首个地址不可用时切换到下一个 |
