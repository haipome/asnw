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
- `kb_*.c` 是已知缺陷的最小复现，预期失败（报告为 XFAIL），只在 asan 变体下运行。
  修复缺陷后把文件改名为 `t_*.c`；如果忘了改名，`run.sh` 会报 XPASS 并让本次运行失败。
- 日志在 `build/<variant>/unittest/log/<name>.log`。
- 测试只使用 127.0.0.1 的临时端口和 `/tmp/asnw-ut-XXXXXX/` 下的 unix socket，可并行、可在容器里运行。
- `nw_svr_release` / `nw_clt_release` 目前有缺陷（见 docs/REVIEW.md 的 H1、H5），通过类用例暂时不调用它们。

## 现有用例

| 用例 | 覆盖 |
|------|------|
| t_buf | nw_buf / pool / list / cache |
| t_sock | 地址解析与 socket 选项 |
| t_timer | 单次、重复、停止 |
| t_state | 增删改查、超时、扩容、迭代 |
| t_job | 4 线程处理 500 个任务，结果回到主线程 |
| t_echo | tcp、udp、unix stream/dgram/seqpacket 回显；stream 约 2MB |
| t_seqpacket_fd | 通过 seqpacket 传递 fd |
| t_clt_reconnect | 服务端未启动时重试、被踢后重连 |
| t_rpc | ut_rpc 请求/应答与心跳保活 |
