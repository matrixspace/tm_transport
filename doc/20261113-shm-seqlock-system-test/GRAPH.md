# 图、MultiTransport 与 fetch 系统测试实施记录

状态：完成（2026-11-13）。leader 集成与当前 HEAD 统一验收通过：图/MultiTransport/fetch **192 PASS / 0 FAIL**（`/tmp/shm-system-leader-final2/graph`）。代码 commit `e7ef37a`（已合入主分支 `1a3abf7`）。2026-11-13，在独立 worktree `shm-system-graph` / 分支 `feature/shm-system-graph`，基线 `b6ff6e7` 开始实施。主记录见 [README](README.md)，需求见 [PLAN](PLAN.md)。

## 方案

独立 executable 使用真实 RealTimeApp/AppRunner、ShmSeqlockImporterExporter 和 MultiTransport 管理器。通过已收到的握手消息确认映射及下游可用；控制面由测试进程条件变量/管道承担。正确性轮次逐条核对 topic、完整 payload 和数量，允许丢包压力轮次另行核算。Python 外部 watchdog 为每个场景分配独立进程，记录退出码和日志；先停止订阅、等待异步动作完成，再析构 runner/environment。

不使用固定 sleep 代替 reader 就绪；启动哨兵可重试，正式报文不重试。性能测量不使用此逐消息握手模式。

## 进度

- 已读取工作区 CLAUDE.md、任务 README/PLAN、参考 AGENT_INDEX、broadcast 和 app-runner 章节，并核对 importer/exporter/fetch 源码接口。

- 构建发现全项目 Meson 在无关的 pkg-config 依赖展开阶段长期等待，改为附带 `standalone/meson.build`；仅探测 tm_basic（传递 tm_infra、boost 等必需依赖）与 threads，编译当前 worktree 的真实组件和 locator。未修改 .pc 或第三方源码。全项目入口保留供最终集成使用。
- 已实现 15 个场景入口：raw_graph、typed_graph、typed_explicit_hooks、multitransport、concurrent、threaded_exporter、shared_exporter、control、slow_sync、slow_async、fetch_raw、fetch_typed、multi_fetch_raw、multi_fetch_typed、synchronous。覆盖 G02/G03/G05/G06/G07 基本 stop/restart/G08/G09 以及 F05/F06。
- `control` 只验证每次可观察的 stop 完成后再 restart，不把异步控制接口没有提供的 FIFO/完成通知当作契约。此时 G04 动态管理尚未实现（后续已补齐）；G07 交错重复命令继续保留为覆盖缺口。

- 首批 debug 冒烟：15 场景中 14 PASS，synchronous 收到全部 20 条后在退出时 SIGSEGV。根因是测试夹具先销毁 Env、随后销毁 SynchronousRunResult；后者析构会通过 importer->control(env,"stop") 使用 Env。修正为先销毁 result，等待 reader 消失，再销毁 Env。此为测试夹具生命周期错误，未修改生产实现或放宽断言。原始证据 `/tmp/shm-graph-debug-smoke/*synchronous*.log`（含结果后异常退出）保留。
- 补齐 dynamic_listener：真实 MultiTransportBroadcastListener facility，经请求/响应图执行 Add/Remove/RemoveAll；每次响应后校验内容、退订静默，再加入。

## 验证结果

工具链：source 工作区 `build-common/build-env-setting.sh`，GCC 15.2.1，Meson 1.11.2；本 worktree 独立 `builddir_graph_debug`，`ninja -j1`，未复用其他 worktree 缓存。

```sh
meson setup builddir_graph_debug test/shm_seqlock_graph_system_test/standalone \
  --buildtype=debug --prefix="$INSTALL_PREFIX" \
  --cmake-prefix-path="$VCPKG_ROOT/installed/x64-linux"
ninja -j1 -C builddir_graph_debug shm_seqlock_graph_system_test
python3 test/shm_seqlock_graph_system_test/run_tests.py \
  builddir_graph_debug/shm_seqlock_graph_system_test --matrix \
  --output /tmp/shm-graph-debug-matrix
```

- debug 全矩阵：16 场景 × 3 layout × 2 read × 2 同 wait 组合 = **192/192 PASS**；原始参数、退出码和时长见 `/tmp/shm-graph-debug-matrix/results.json`，逐例日志同目录。编译日志 `/tmp/shm-graph-standalone-build-final.log`。所有正式测试校验无损坏/重复/逆序，bounded 场景精确收齐。
- 末轮审查为 `control` 添加停止前的已确认哨兵，将 latest 合法重放与业务区间隔离；对这项变化单独重复参数矩阵，**60/60 PASS**（12 参数 × 5 次，每次 20 个停止/重启循环，共 1200 个循环）；原始结果 `/tmp/shm-graph-debug-control-repeat/results.json`。
- Python `py_compile` 与 `git diff --check` 通过；格式化使用本仓库 `.clang-format`。
- 本子任务未运行 release、sanitizer、长跑或性能测试；这些结果由 leader 统一执行记录。未发现需要修改生产代码的缺陷。初次 synchronous 崩溃为已修正的夹具退出顺序错误。

## 剩余覆盖边界

G04 已覆盖动态加入/移除/重加与 RemoveAll 响应；尚未测试替换为另一个业务段地址。G07 尚未覆盖重复/交错异步控制命令。G09 验证同步与异步积压位置，超过环容量的 slow-reader 套圈边界由主系统控制面承担。slotCount=8192 用于界定并发场景最多 1000 条积压，不作为小环套圈验证。
