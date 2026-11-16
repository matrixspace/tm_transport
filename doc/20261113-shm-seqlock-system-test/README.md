# ShmSeqlockComponent 系统测试

**任务开始时间：** 2026-11-13
**任务结束时间：**（未结束）
**状态：** 进行中（功能、性能、多路稳定性验收已完成；DEF-1 决定**不修复**；待 graph/MultiTransport 性能、单配置 soak、推送/参考文档同步）
**分支：** `feature/shm-seqlock-system-test`（代码 HEAD `727f88d`）
**代码基线：** `ccb9a8c`（从 `develop` 创建）
**worktree：** `worktrees/shm-seqlock-system-test`（相对工作区根目录）

## 背景

用户要求制定并后续实施 ShmSeqlockComponent 的系统测试，充分覆盖功能、效率、鲁棒性，以及非图调用、数据流图、Heartbeat、慢 reader、频繁连接断开、writer 崩溃脏数据、多 writer 等实际用法。

已有 `shm_seqlock_test` 覆盖基本收发、部分过滤、慢 reader、随机 SIGKILL 重启、超限、futex 和 fetch；`ipc_concurrency_test` 覆盖多线程、多进程和部分图场景。缺少精确崩溃窗口、真实 Heartbeat/发现及完整生命周期验证。

用户已确认：允许加入**仅测试构建启用**的故障注入点，生产构建不包含注入逻辑。

## 交接摘要（2026-11-13）

### 当前结论

- 测试设施与五个套件（core / graph / heartbeat / resource / baseline）均已实现，并在当前 HEAD 上完成统一复跑。
- 支持用法的功能、图集成、真实 Heartbeat/发现、既有基线全部通过。
- 唯一失败为生产组件已知资源缺陷 **DEF-1**：callback 从自己的 reader 线程移除订阅后，订阅对象和段映射保留到组件析构。2026-11-13 用户决定**不修复**，作为已知限制；测试保留严格断言与证据，未修改生产代码、未放宽断言。`run_suite.py` 整体退出码为 1，属长期预期结果。
- 本轮未执行：正式五轮性能矩阵、2h/24h 长跑、资源耗尽/`/dev/shm` 满等剩余鲁棒性项、TSan；未推送、未合并回 `develop`、未同步 `tm_kit_reference`。夜间 20 次全窗口重复已在最终 worker 上执行（结果见下文）。

### 权威验证结果（当前 HEAD）

| 套件 | 构建 | 入口 / 命令 | 结果 | 证据目录 |
|---|---|---|---|---|
| 核心功能、精确崩溃、futex、套圈、黑盒，以及 F10/F11/R03/R05/R06/R07/R08/R-dirty-ext/R-risk/R-resource | release | `run_tests.py --matrix` | **778 PASS / 0 FAIL** | `/tmp/shm-func-matrix2` |
| 同一核心矩阵 | debug | 同上 | **778 PASS / 0 FAIL** | `/tmp/shm-func-debug-final` |
| 同一核心矩阵 | ASan+UBSan debug（`detect_leaks=0`） | 同上 | **778 PASS / 0 FAIL**（R-resource 的 fd 上限探测在 sanitizer 下 SKIP，见说明） | `/tmp/shm-func-asan-final2` |
| F09 重构后夹具（历史单独补跑） | ASan+UBSan debug（`detect_leaks=0`） | `--case F09 --matrix` | **24 PASS / 0 FAIL** | `/tmp/shm-system-asan-parameters` |
| 夜间重复 | release | `--matrix --repeat=20` | **15,560 PASS / 0 FAIL** | `/tmp/shm-nightly-repeat20-final` |
| 进程重连（1000 次） | release | `--case R08`（`SHM_R08_CYCLES=1000`） | **PASS** | `/tmp/shm-r08-1000-final` |
| 图 / MultiTransport / typed / hook / fetch | release | graph runner | **192 PASS / 0 FAIL** | `/tmp/shm-suite-final/graph` |
| 真实 Heartbeat 与发现 | release | heartbeat runner | **30 PASS**；H04 风险探测本轮 1×RISK_OBSERVED、2×RISK_NOT_OBSERVED（跨轮三布局均出现过；风险不计 PASS） | `/tmp/shm-suite-final/heartbeat` |
| 资源、性能 smoke、oracle | release | resource runner | **18 PASS / 1 FAIL**（仅 DEF-1） | `/tmp/shm-suite-final/resource` |
| 既有基线：3 layout + IPC 并发 | release | baseline | **4/4 PASS** | `/tmp/shm-suite-final/baseline-*` |

统一入口 `run_suite.py --matrix --baseline` 的总退出码为 1，只因 resource 的 DEF-1（决定不修复，长期预期）；核心、图、Heartbeat、基线均为 0。各子套件详细说明见 [GRAPH.md](GRAPH.md)、[HEARTBEAT.md](HEARTBEAT.md)、[RESOURCE.md](RESOURCE.md)。版本化汇总（命令、环境、二进制 SHA256、各套件计数、DEF-1 定位）见 [UNIFIED_RESULTS.json](UNIFIED_RESULTS.json)。

### 下一步（按优先级）

DEF-1 已决定不修复（见下文），resource smoke 保留 1 个预期 FAIL，`run_suite.py` 退出码 1 是长期预期状态。其余待办：

1. **剩余性能与长跑**：graph/MultiTransport 性能、重连/接管时延、单配置 2h/24h soak。staged、饱和、50%/90%、全量 1200 配置矩阵和多路稳定性均已完成；稳定性因用户确认时长足够，于 2026-11-16 09:50 在 48.7h 停止（10/10 PASS，[STABILITY_RESULTS.json](STABILITY_RESULTS.json)）。
2. **剩余鲁棒性**：`/dev/shm` 满/空间不足、mmap 失败、权限组合、外部篡改已提交 payload（PLAN 已明确 not a guarantee）；FD 上限与 mlock 不可用已由 R-resource 覆盖。
3. **推送与合并**：合并本分支回 `develop` 并推送；随后删除 `feature/shm-system-{graph,heartbeat,perf}` 子分支与 worktree。
4. **参考文档**：推送后按工作区规范更新 `tm_kit_reference` 的 shm_seqlock、Heartbeat、并发章节并关联本任务。
5. **TSan / LeakSanitizer**：当前 ptrace 环境 LeakSanitizer 报 fatal error，只能用 `detect_leaks=0`；TSan 尚未执行，且不能证明跨进程共享内存无撕裂。

### 快速复跑

```sh
cd /data/hxli/codes/tm_kit/build-common && source ./build-env-setting.sh
cd /data/hxli/codes/tm_kit/worktrees/shm-seqlock-system-test

meson setup builddir_suite_release test/shm_seqlock_system_test/suite --buildtype=release
ninja -j4 -C builddir_suite_release
python3 test/shm_seqlock_system_test/run_suite.py builddir_suite_release \
  --matrix --baseline --out=/tmp/shm-suite-$(date +%s)
```

预期：core / graph / heartbeat / baseline PASS，resource 仅 `churn selfRemove=true` 轮 FAIL（DEF-1），整体退出码 1。`--only core|graph|heartbeat|resource|baseline` 可重复选择；输出目录必须不存在。

ASan/UBSan 单独构建：

```sh
meson setup builddir_system_asan test/shm_seqlock_system_test/standalone \
  --buildtype=debug -Db_sanitize=address,undefined \
  -Dtm_include="$INSTALL_PREFIX/include" \
  -Dboost_include="$VCPKG_ROOT/installed/x64-linux/include"
ninja -j4 -C builddir_system_asan
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
python3 test/shm_seqlock_system_test/run_tests.py \
  --binary builddir_system_asan/shm_seqlock_system_worker \
  --matrix --log-dir=/tmp/shm-asan-unique
```

### 已知缺陷 DEF-1（决定不修复）：callback 自移除后资源保留到组件析构

**现象。** callback 在自己的 reader 线程内调用 `shm_seqlock_removeSubscriptionClient()` 移除最后一条订阅后，组件仍存活期间每次自移除都保留一份段映射和已退出但未 join 的线程对象；只有组件析构才统一释放。

**量化证据**（release、`--mode=churn --cycles=10000 --wait=futex --selfRemove=true`，见 [RESOURCE_RESULTS.json](RESOURCE_RESULTS.json)）：

| 指标 | 基线 | 组件存活（10k 次后） | 析构后 |
|---|---:|---:|---:|
| 段映射数（含 writer） | 1 | **10,001** | 1 |
| 总映射数 | 42 | **30,044** | 52 |
| RSS KiB | 2,124 | **159,628** | 39,684 |
| FD / 线程数 | 5 / 1 | 5 / 1 | 5 / 1 |
| 普通（非自移除）对照 | 1 | 1 | 1 |

短 smoke 中第 18 轮（20 次自移除）同样 FAIL；统一套件可稳定复现。`threadHandles()` 不包含已停止项，数据面交付正确，问题只在资源回收时机。

**根因。** `src/shm_seqlock/ShmSeqlockComponent.cpp`：

- `:1100-1101` `stoppedSubscriptions_` 的注释与容器；
- `:1151-1157` `potentiallyStopSubscription()` 在检测到“当前线程就是该订阅的 reader 线程”时，把 `unique_ptr` 移入 `stoppedSubscriptions_`，而不是就地销毁；
- `src/shm_seqlock/ShmSeqlockComponent.cpp:983-993` 订阅析构才 join 线程并 `map_.unmap()`；
- `:1212-1223` 组件析构才 `stopped.swap(stoppedSubscriptions_)` 并在锁外释放。

就地销毁不可行：reader 线程正在执行 callback，销毁会自 join 并 unmap 仍在使用的映射，因此需要延后；当前实现把“延后”做成了“延迟到组件析构”，缺少中间回收点。

**影响。** 数据正确性、线程数、FD 不受影响；长生命周期组件 + 一次性/自移除订阅（受支持的 R06 用法）会让映射与 RSS 随自移除次数线性增长。

**为什么保持 FAIL。** PLAN 要求“现有缺陷保留失败例和证据；修复另行决定，不悄悄改变契约”。`run_suite.py` 不会豁免该失败，汇总退出码非零是设计行为。

**最终决定（2026-11-13）：不修复。** 方案 A 已完成实现与验证，但用户判断其对其它代码路径影响较大，要求回滚；随后决定接受该已知限制，生产代码保持基线。测试继续保留失败断言与证据，resource smoke 的 1 个 FAIL 和统一入口退出码 1 属**长期预期状态**，不视为测试设施失败。受影响用法需在业务侧规避（见“兼容性与注意事项”）。

**候选修复方向（备查；当前不实施）**：

1. **机会式回收**：任一非 reader 线程的组件公共调用（add/remove/stats/threadHandles/publisher 等）在锁外 swap 并清空 `stoppedSubscriptions_`。改动小，但纯自移除且不再调用组件的场景仍保留到析构。
2. **组件级 reaper**：组件持有一个回收线程/共享回收队列，reader 线程退出后尽快 join 并 unmap；回收及时，但增加线程与生命周期管理，需评估 API/ABI 影响（不改变现有接口）。
3. **显式契约**：承认“自移除后的回收发生在下一次组件操作或析构”，同时放宽测试断言；不推荐，除非确认业务可接受无界增长。

三种方案都不改变共享内存布局和公共 API；建议单独立项修复，并保留本测试作为回归门槛。

**已尝试并回滚（方案 A）**：2026-11-13 曾实现机会式回收 `reapStoppedSubscriptions()`，并在 `addSubscriptionClient`、`removeSubscriptionClient`、`getPublisher`、`threadHandles`、`readerStats`、`publisherStats` 中调用。验证结果：resource smoke 19/19；10k 自移除段映射 1、总映射 46、RSS 3.9MB（修复前 10,001 / 30,044 / 159.6MB）；统一套件全部 PASS、退出码 0。用户评估后认为该方案**对其它的代码路径影响较大**，要求回滚；生产源码已恢复基线，未产生代码提交。实验数据与前后对照保留在 [DEF1_FIX_EXPERIMENT.json](DEF1_FIX_EXPERIMENT.json)，不计入当前交付结果。后续若重启修复，应优先评估对图/Heartbeat/并发退出路径的影响，并保留 resource smoke 作为回归门槛。

## 当前交接与恢复入口

2026-11-13 用户已指示在本 worktree 开始执行；此前仅提交文档后停止的交接步骤已完成。上下文被压缩或会话中断后，先读本 README、[PLAN](PLAN.md) 和各子任务记录确认进度，再检查本 worktree 的 `git status`、`git diff`、`git log -3`。

环境与约束：

- 工作区根目录为 `/data/hxli/codes/tm_kit`；进入本 worktree 开发，禁止在原仓库 `develop` 工作目录修改。
- 已阅读工作区 `CLAUDE.md`、用户提供的 AGENTS.md、`tm_kit_reference/AGENT_INDEX.md`；当前目标仓库未发现额外 CLAUDE.md/AGENTS.md。恢复后修改前再次核对目标路径规则。
- 先读参考索引，再按主题逐篇阅读；源码优先于文档。不要猜测第三方 API，需要时按 AGENTS.md 使用 Context7。
- 构建前 source 工作区 `build-common/build-env-setting.sh`，确认 gcc-toolset-15；本 worktree 使用自己的构建目录，禁止复用原仓库 Meson 缓存。ninja 始终限制 `-j4` 或更少，只编译相关目标。
- 不修改 vcpkg/第三方库；不在 master 提交或合并；不提交 `.codex/`、`.claude/`。
- 准备阶段未运行测试；实施阶段的实际结果见上文与各子任务记录。未执行的方案不得记为通过，风险探测不得混作 PASS。
- 不兼容 API/布局变更先询问用户；测试发现缺陷要保留证据，不能悄悄改变契约或放宽断言。

## 方案（含被放弃的方案和原因）

完整且可指导实现与执行的方案见 [PLAN.md](PLAN.md)。按基础设施、功能/图集成、真实 Heartbeat、确定性故障、性能/长跑推进，复用已有测试资产。

不单靠随机 SIGKILL：不能证明覆盖了提交前后等关键窗口；采用测试专用屏障注入，并保留生产构建黑盒随机故障。

不单靠组件 `corrupt`：该字段不校验 payload 完整性；测试必须独立逐字节/校验值验证。

不把未加锁并发写的偶然成功当安全保证；风险探测与支持用法分别报告。

## 代码改动（按文件列出）

已实现独立测试设施与程序：

- `doc/20261113-shm-seqlock-system-test/README.md`：任务背景、交接摘要、结果、已知缺陷与进展。
- `doc/20261113-shm-seqlock-system-test/PLAN.md`：完整测试方案。
- `doc/INDEX.md`：任务索引入口。
- `src/shm_seqlock/TestProbe.hpp`、`ShmSeqlockComponent.cpp`：仅宏启用的内部事件及分段拷贝屏障；不安装接口，不改公共 API 或段布局。
- `test/shm_seqlock_system_test/`：进程控制 worker、逐字节 oracle、确定性故障/功能矩阵、无注入黑盒、最小/集成 Meson 工程、leader 汇总入口 `run_suite.py`。
  - `SystemTestWorker.cpp` 新增 F10 查询压测、R05 并发移除、F11 CPU provider、R06 callback 控制、R03 多 callback、抛异常 callback、悬垂 publisher 等角色，以及 `ADDP`/`RUN`/`RELEASE`/`REPORT` 命令。
  - `run_tests.py` 新增 F10/F11/R03/R05/R06/R07/R08/R-dirty-ext/R-risk/R-resource 用例；每例结果增量写入 `results.jsonl`（修复 `--repeat=20` 的 O(n²) 全量重写）；生命周期用例在矩阵中只跑默认线组合。
- `test/shm_seqlock_graph_system_test/`：图、MultiTransport、typed/hook、fetch、同步/动态管理、慢下游；见 [图测试记录](GRAPH.md)。
- `test/shm_seqlock_heartbeat_system_test/`：真实心跳/alert/custom、发现、TTL、timer、锁竞争、容量；见 [心跳测试记录](HEARTBEAT.md)。
- `test/shm_seqlock_resource_system_test/`：无注入性能/资源/churn及显式长跑入口；`aggregate_perf.py` 将多个 `--profile=full` 批次聚合为版本化基线；见 [资源与性能记录](RESOURCE.md)。
- `test/meson.build`：注册上述测试目录；原有生产 library 不设置故障注入宏。

## 测试与验证（命令、构建类型、结果）

准备阶段仅做只读调查和建分支。以下为实施阶段实际执行结果的汇总；构建环境为 GCC 15.2.1、Meson 1.11.2、release/debug 各自的独立 builddir。

### 核心矩阵（release / debug / sanitizer）

```sh
# release（统一套件内执行）
python3 test/shm_seqlock_system_test/run_tests.py \
  --binary builddir_suite_release/shm_seqlock_system_worker \
  --seed 20261113 --matrix --log-dir /tmp/shm-system-leader-final2/core

# debug 单独构建
python3 test/shm_seqlock_system_test/run_tests.py \
  --binary builddir_system_debug/shm_seqlock_system_worker \
  --matrix --log-dir /tmp/shm-system-debug-final

# ASan+UBSan（LeakSanitizer 在当前 ptrace 环境不可用）
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
python3 test/shm_seqlock_system_test/run_tests.py \
  --binary builddir_system_asan/shm_seqlock_system_worker \
  --matrix --timeout 120 --log-dir /tmp/shm-func-asan-final2
```

结果：release、debug、ASan+UBSan 三个构建在**当前 HEAD** 上均为 **778 PASS / 0 FAIL**（ASan 下 R-resource 的 fd 上限探测因 ASan runtime 自身需要描述符而 SKIP，mlock 探测仍执行；该失败路径由 release/debug 覆盖）。覆盖 24 组核心参数、指定崩溃与恢复窗口、futex 登记/睡眠交错、套圈精确丢失统计、全部有向布局转换、无注入黑盒，以及新增的功能/异常用例。

### 集成套件

```sh
python3 test/shm_seqlock_system_test/run_suite.py builddir_suite_release \
  --matrix --baseline --out=/tmp/shm-system-leader-final2
```

结果：core PASS（778）、graph PASS（192）、heartbeat PASS（30 PASS + 3 风险探测）、resource FAIL（18 PASS/1 FAIL，DEF-1）、baseline 4/4 PASS；证据 `/tmp/shm-suite-final`。生产归档 `nm -C builddir_suite_release/libproduction_shm_seqlock.a` 无 `systemTestProbe`/`systemTestCopy`，instrumented 归档有预期未定义引用，确认生产/性能路径不含注入。

### 功能与异常覆盖补齐（2026-11-13）

按 PLAN 第 3、4 节补齐此前缺失的功能与异常用例，全部接入 `--matrix` 和 `--case`；这些用例与 24 组线格式组合正交，矩阵中只在默认组合运行一次：

| ID | 用例 | 结果 |
|---|---|---|
| F10 | 4 个查询线程并发调用 reader/publisher stats 与 threadHandles，同时 100 次 add/publish/remove | PASS；结束 handles=0 |
| F11 | CPU lease provider 缺失/成功/重复安装/抛异常四种路径 | PASS；缺失与绑定失败均明确抛错且同组件可回退创建 |
| R03 | 同段慢/快 callback 共享 reader 线程 | PASS；慢 callback 阻塞期间 fast=0，释放后 3 条全部交付 |
| R05 | 4 线程并发 add、同时 remove（含重复/未知 id）20 轮 | PASS；无残留 reader、无死锁/UAF |
| R06 | callback 移除其他订阅；自移除后从 reader 线程重新订阅 | PASS；A 收 5/5、B≤1；自移除后 A=1、C=3 |
| R07 | 最后订阅退出后以不同 layout/read/wait 重新添加 | PASS；重复/未知 remove 安全 |
| R08 | reader 进程重连 churn（默认 200，`SHM_R08_CYCLES=1000` 已跑满 1000 次），每 50 次 SIGKILL writer 并接管 | PASS |
| R-dirty-ext | 结构合法但字段异常的头：valid/odd seq/超长长度/错误 topicHash/published 越过环 | PASS；reader 不崩溃且可正常退出，新段恢复 |
| R-risk | callback 抛异常、publisher 捕获对象超出组件生命周期、活跃映射 truncate、替换锁文件 | 风险探测 PASS（结果见下） |
| R-resource | `RLIMIT_NOFILE=5` 下 writer 明确失败；`RLIMIT_MEMLOCK=0` 下 mlock 失败但读写仍正常 | PASS；fd 上限给出 "Too many open files"，mlock 为 best-effort |

完整功能矩阵（含上述新用例）`--matrix` 为 **778 PASS / 0 FAIL**（`/tmp/shm-func-matrix2`）。夜间 `--repeat=20` 共 **15,560 PASS / 0 FAIL**（`/tmp/shm-nightly-repeat20-final`），在无并发构建干扰的最终 worker 上完成；此前一次重复因并行重建 worker 导致 `PermissionError`，该轮已作废并重跑。

风险探测观察到的事实（**不构成安全保证**）：

- callback 抛异常：reader 进程 SIGABRT（`-6`），组件不捕获用户异常。
- publisher 捕获函数在组件析构后调用：进程 SIGSEGV（`-11`）。
- 活跃映射被外部 `truncate`：writer 和 reader 均死亡（SIGBUS）。
- writer 持锁期间替换 `.lock` 文件：第二个 writer 可以成功 READY（两个 writer 共存风险）。
- `published` 被伪造成 `2^63`：reader 由于 `2*next+2` 回绕会误读 slot 0 并交付 1 条消息（`RISK published_huge_delivered=1`）。

说明：R02（不同组件/进程快慢 reader）由现有 `shm_seqlock_test` 的慢 reader 轮次和资源 smoke 的 0/2/4/8 reader 轮次覆盖，未重复实现；F06（typed 非法编码/错误类型/截断）由图套件的 `typed_graph`/`typed_explicit_hooks` 覆盖。FD 上限与 mlock 不可用已由 R-resource 覆盖；尚未补齐的 4.4 项是 `/dev/shm` 满、mmap 失败、权限组合和外部篡改已提交 payload 的完整性语义（PLAN 已明确 seqlock 不提供该保证）。

### 性能测试（2026-11-13 起，进行中）

按用户要求，性能与长跑在功能/异常用例完成后开始。已完成的 release 性能批次包括 **staged 基线、饱和吞吐、50%/90% 负载和全量 1200 配置矩阵**：

- 构建：`builddir_suite_release`，release，无故障注入（`--mode=environment` 确认 `build_type=release`、`fault_probes=false`）。
- 机器：Intel Xeon Gold 5117 @ 2.00GHz，56 逻辑 CPU、2 NUMA 节点；内核 4.18；`scaling_governor`/`scaling_cur_freq` sysfs 不可用。
- 绑定：`taskset -c 16,18,20,22,24,26,28,30,32,34`（NUMA0 的 10 个物理核）。
- 每配置 5 轮 ×（5s 预热 + 30s 测量），随机顺序，10,000 offered/s。
- 批次 A：readers 0/1/2/4/8 × payload 256 × inline/classic × spin/futex。
- 批次 B：readers 1 × payload 64/256/1024/4096 × inline/classic × spin/futex。
- 批次 C：readers 1 × payload 256 × layout inline/split/inline-premark × read classic/spec × spin/futex。
- 驱动修正：`performance_baseline` 现在只在全部默认维度（readers 0,1,2,4,8；payload 64,256,1024,4096；3 layout × 2 read × 2 wait；rate 10000）时置 true，子集批次明确为 false。
- 结果：3 个批次共 150 轮（含 oracle）全部 PASS；26 个唯一配置，每 reader 每配置 5×300,000=1,500,000 样本，**所有配置 lost=0、bad=0、order=0、rejected=0**。版本化结果 [PERF_BASELINE.json](PERF_BASELINE.json)，聚合工具为 `test/shm_seqlock_resource_system_test/aggregate_perf.py`。

代表性结果（10k offered/s，5 轮均值）：

| 场景 | publish 均值 | publish p99 | reader p50 | reader p99 | 进程 CPU |
|---|---:|---:|---:|---:|---:|
| 0 reader, spin | 96 ns | 160 ns | — | — | 99.8% |
| 0 reader, futex | 112 ns | 186 ns | — | — | 99.8% |
| 1 reader, 64B, spin | 93 ns | 135 ns | 349 ns | 505 ns | 200% |
| 1 reader, 256B, spin, inline | 105 ns | 252 ns | 462 ns | 902 ns | 200% |
| 1 reader, 256B, futex, inline | 2.35 µs | 2.72 µs | 4.87 µs | 5.64 µs | 170% |
| 1 reader, 1KiB, spin | 144 ns | 315 ns | 741 ns | 1.01 µs | 200% |
| 1 reader, 4KiB, spin | 657 ns | 1.12 µs | 1.67 µs | 2.16 µs | 200% |
| 1 reader, 256B, spin, split | 101 ns | 210 ns | 446 ns | 637 ns | 200% |
| 1 reader, 256B, spin, inline-premark | 108 ns | 169 ns | 404 ns | 584 ns | 200% |
| 1 reader, 256B, read=spec, spin | 96 ns | 250 ns | 399 ns | 709 ns | 200% |
| 2 readers, 256B, futex | 3.22 µs | 3.55 µs | 5.20 µs | 6.05 µs | 244% |
| 4 readers, 256B, futex | 5.07 µs | 5.54 µs | 6.62 µs | 8.19 µs | 378% |
| 8 readers, 256B, futex | 8.29 µs | 9.37 µs | 7.08 µs | 10.8 µs | 658% |
| 8 readers, 256B, spin | 109 ns | 378 ns | 754 ns | 1.13 µs | 898% |

慢 reader 下 writer 成本（`shm_seqlock_test` release，阈值 1.5×base+50ns）：inline 142.5→138.8 ns、split 146.1→147.0 ns、inline-premark 140.1→147.2 ns，**全部远低于阈值**。futex 空闲 reader CPU：0.0–0.2%（spin publisher 对照 0.8%，busy-spin 对照约 100%），**低于 5% 门槛**。inline-premark 的 `testNoPremark` spec 负对照本轮未观察到撕裂（torn=0），按 PLAN 记为“不充分”，不是硬失败；同一负对照在功能套件的 early run 曾观察到 torn=152。

**饱和吞吐（`rate=0`，5 轮×30s，readers 0/1/2/4/8 × spin/futex，payload 256 inline/classic）：** 50 轮全部 PASS，所有配置**没有丢包**——在这些配置下 writer 自身先达到吞吐上限，而不是 reader 跟不上：

| readers | spin offered/s（最小–最大） | futex offered/s（最小–最大） |
|---:|---|---|
| 0 | 4.49M–4.57M | 3.95M–4.04M |
| 1 | 3.74M–3.82M | 2.69M–2.71M |
| 2 | 2.73M–3.05M | 2.27M–2.38M |
| 4 | 2.12M–2.30M | 1.95M–2.18M |
| 8 | 1.66M–1.73M | 1.58M–1.65M |

**50%/90% 负载（5 轮×30s，payload 256 inline/classic）：** 8 个配置全部 PASS、零丢失。50% 轮次全部精确达到请求速率；90% 轮的 1 reader 实际略低于请求（spin 3.27M vs 3.37M，futex 2.33M vs 2.42M），说明已接近 writer 上限，但 reader 仍无丢失：

| 配置 | 请求/s | 实际/s | loss | reader p50 | reader p99 |
|---|---:|---:|---:|---:|---:|
| 1 reader spin 50% | 1.87M | 1.87M | 0 | 0.47 µs | 0.55 µs |
| 1 reader spin 90% | 3.37M | 3.27M | 0 | 0.38 µs | 0.76 µs |
| 1 reader futex 50% | 1.34M | 1.34M | 0 | 0.49 µs | 0.57 µs |
| 1 reader futex 90% | 2.42M | 2.33M | 0 | 0.47 µs | 0.58 µs |
| 8 readers spin 50% | 0.83M | 0.83M | 0 | 0.71 µs | 0.88 µs |
| 8 readers spin 90% | 1.50M | 1.50M | 0 | 0.74 µs | 1.12 µs |
| 8 readers futex 50% | 0.79M | 0.79M | 0 | 0.72 µs | 0.90 µs |
| 8 readers futex 90% | 1.42M | 1.42M | 0 | 0.73 µs | 1.02 µs |

版本化负载数据：[LOAD_RESULTS.json](LOAD_RESULTS.json)。

**全量 1200 配置矩阵（2026-11-14 07:17 完成）：** 按 layout 分 3 块，每块 401 项（400 perf + oracle）全部 PASS；共 **240 个唯一配置 × 5 轮 = 1200 轮**，覆盖 readers 0/1/2/4/8 × payload 64/256/1024/4096B × 3 layout × classic/spec × spin/futex，10k offered/s。所有配置 **lost=0、bad=0、order=0、rejected=0**；publish 均值 84 ns–9.29 µs，p99 111 ns–11.6 µs。版本化数据 [PERF_FULL_RESULTS.json](PERF_FULL_RESULTS.json)，原始证据 `/tmp/shm-perf-full-{inline,split,inline-premark}/`。

待完成：graph/MultiTransport 性能；单配置 2h/24h soak（多路 72h 稳定性已另行启动）。其余原始证据在 `/tmp/shm-perf-stage-{a,b,c}/`、`/tmp/shm-perf-saturation/`、`/tmp/shm-perf-load-*` 与 `/tmp/shm-perf-base-{inline,split,inline-premark}.log`。

### 长时间稳定性测试（2026-11-13 起，待全量性能矩阵结束后启动）

新增 `test/shm_seqlock_stability_test/`：自驱动 stability worker（生产组件、无注入）+ 多路 Python 编排器。writer 按速率发送带 generation/序号/大小/校验和的报文，reader 校验完整性/顺序/丢失/重复/乱序/损坏；慢 reader 在 callback 中 sleep；Python 负责多 lane 并发、随机 SIGKILL/重启 writer 与 reader、资源采样、进度与汇总。

默认 10 个 lane，覆盖用户要求：固定/变长 payload（64B、64–4096B、4KiB、1MiB）、低/高频（100/s、1k/s、2k/s、10k/s、20k/s、100k/s）以及 **`rate=0` 最大吞吐变长压力**、少/多 reader（2/3/4/8）、writer 与 reader 随机重启、快/慢 reader（0/2ms/5ms）、inline/split/inline-premark、slotSize 128B–2MiB、slotCount 2–4096、stride128、mlock、writerLock。判定：所有 reader 的 corrupt/bad/dup/ooo 必须为 0；无重启 lane 的 fast reader 不允许丢失；慢 reader 丢失只报告；restart lane 和最大吞吐 lane 允许丢失但必须无损坏/乱序；writer rejected 必须为 0。

实现状态：worker、编排器、standalone Meson 和文档已完成。全量性能矩阵结束后已构建并跑两次短冒烟（5 分钟全 lane、2 分钟高频/tiny-ring），修复了两个夹具问题：writer 重启时 generation 需每进程唯一；高频速率控制在小间隔下改用自旋（`sleep_for` 粒度不足）。第二次 5 分钟全 lane 冒烟 **9/9 lane PASS**；高频 lane 达到 100k/s，tiny-ring 慢读者产生预期丢失而快读者零丢失。

**资源监控**：编排器自身只保留 RSS/PSS/线程/映射/FD 的最大值。为不中断运行中的测试，新增独立 `test/shm_seqlock_stability_test/resource_monitor.py`：按 runner PPID 发现 worker，每 30s 记录 RSS/PSS/VmSize/HWM/Swap、CPU%、线程、映射、FD 分类、缺页、上下文切换、IO 和 smaps 明细；每条记录带 lane/role/reader index/pid/seed/slow_us 和实验时间轴 `exp`，可与 `progress.json`、runner 汇总和实例日志关联。输出 `/tmp/shm-stability-72h-v2-resources/{resources.jsonl,progress_snapshots.jsonl,resource_summary.json}`。

**多路稳定性最终结果（10 lane，2026-11-14 09:09 启动，2026-11-16 09:50 按用户要求停止，运行 48.7h）：**

- 9 lane 版本 07:30 启动，09:08 增加 `ultra-var-max`（变长 64–4096B、`rate=0` 最大吞吐、8 快 reader）后优雅停止（运行 1.6h，9/9 PASS，汇总在 `/tmp/shm-stability-72h/summary.json`），09:09 以 10 lane 重新启动。
- **10/10 lane PASS，0 violations**。总发送 **37,666,054,606** 条；reader 累计接收 **183,174,608,960** 条；corrupt/bad/dup/ooo **全为 0**；writer 实例 838、reader 实例 1185。
- 丢失 669,599,831 条全部来自慢 reader / 重启窗口：restarts 263.6M、slow-vs-fast 315.9M、tiny-ring 90.0M；所有 no-restart fast reader 零丢失（tiny-ring fast 仅 7/10 条 slotCount=2 边界丢失）。
- 随机重启：writer 828 次、reader 1147 次，全部恢复且无损坏/乱序。
- 版本化数据：[STABILITY_RESULTS.json](STABILITY_RESULTS.json)、[STABILITY_SUMMARY.json](STABILITY_SUMMARY.json)（原始汇总）、[STABILITY_RESOURCE_RESULTS.json](STABILITY_RESOURCE_RESULTS.json)；原始 `/tmp/shm-stability-72h-v2/` 与 `/tmp/shm-stability-72h-v2-resources/`。
- 资源（48.7h，272,633 条采样、1,973 个 PID；43 个长期 PID 各 ≥1000 样本）：**max ΔRSS=0、ΔPSS=+6 KiB、ΔFD=0、Δ线程=0、Δ映射=0**；FD writer 4 / reader 3；large-slot RSS 136 MB 稳定；高频 lane CPU ~99.7%/进程，低频/futex lane ~0.1–1%。

### 失败、SKIP 与风险

- **DEF-1**：resource `churn selfRemove=true` 稳定 FAIL，证据见上；不放宽断言。
- **Heartbeat H04 风险探测**：inline-premark 观察到非法/逆序，inline/split 未观察到；首轮 Release 与追加风险证据轮在三个 layout 均至少出现过一次，属随机时序，“未观察到”不等于安全，不计入 PASS。
- **LeakSanitizer**：无法运行，只能 `detect_leaks=0`；不宣称泄漏检查通过。
- 未执行的规格见“下一步”。

## 兼容性与注意事项

- 生产构建的事件宏消除，分段拷贝宏展开回原来的单次 memcpy；公共 API、ABI、共享内存布局不变。
- 系统测试仅 Linux；传输节点只支持 RealTimeApp，不直接放入单遍回测 App。
- 一个组件同段共用读线程，慢 callback 会影响该段其他 callback。
- sender/reader 首次创建属性生效；后续 locator 上加 writerLock 不能改变既存 sender。
- 故障注入仅测试构建，正式性能测量用无注入的 release 构建。
- **DEF-1（决定不修复）**：自移除订阅的资源回收延迟到组件析构。生产使用时不要依赖“长生命周期组件 + 大量自移除订阅”的组合；若业务必须高频自移除，应改为由其他线程移除，或让组件随业务对象一起析构。

## 相关文档

- [**测试报告（功能 / 效率 / 稳定性）**](REPORT.md)
- [完整测试方案](PLAN.md)
- [图、MultiTransport 与 fetch 测试记录](GRAPH.md)
- [真实 Heartbeat 与发现测试记录](HEARTBEAT.md)
- [资源与性能测试记录](RESOURCE.md)
- [staged 性能基线数据](PERF_BASELINE.json)
- [饱和与 50%/90% 负载数据](LOAD_RESULTS.json)
- [全量 1200 配置性能数据](PERF_FULL_RESULTS.json)
- [统一入口说明](../../test/shm_seqlock_system_test/README.md)
- [已有 IPC 并发测试](../20261103-ipc-concurrency-test/README.md)
- [已有 IPC 并发修复](../20261103-ipc-concurrency-bugfix/README.md)
- [线程绑核任务](../20261112-ipc-thread-affinity/README.md)
- 参考仓库（相对工作区）：`tm_kit_reference/AGENT_INDEX.md`、`tm_transport/03-transports/shm_seqlock.md`、`tm_transport/05-heartbeat-and-alert.md`、`tm_transport/01-broadcast.md`、`tm_transport/08-concurrency.md`。

参考仓库不在本 Git 仓库内部，故使用工作区相对路径描述，避免 worktree 与普通 checkout 间产生失效的相对链接。开发测试完成且推送后，按工作区规范更新相关参考文档并关联本任务。

## 实施进展

- 2026-11-13：重新阅读 README、PLAN、工作区 CLAUDE.md 与参考索引、shm_seqlock 全章；目标路径无额外规则。分支正确，`git status`/`git diff` 干净，最新文档提交 `b6ff6e7`。确认 GCC 15.2.1、Meson 1.11.2 和 clang-format 可用。
- 基础设施方案：测试专用 executable 单独编译组件源文件，在该 target 上启用宏；生产 library 不带宏。屏障事件由独立控制通道回报；先用真实 reader 映射事件证明起点，再发布受控报文。
- 构建调查：worktree 的 certify 子模块未初始化，已从主仓库离线克隆并检出 gitlink `b74d50d6`，未修改第三方。完整 Meson 配置卡在 `pkg-config --cflags libetcdcpp`，独立 `timeout 10` 查询同样耗尽 10 秒 CPU 并超时（日志 `/tmp/shm-system-etcd-cflags.log`）。停止无关依赖探测，新增最小 standalone Meson 工程；源码来自本 worktree、头文件来自已安装 infra/basic 与 vcpkg，不复用其他 Meson 缓存。
- 用户授权多个子 agent 分别开发独立程序。已建立图、Heartbeat、资源/性能三个独立分支/worktree，leader 负责控制面、故障注入与最终统一验证。
- 首轮故障驱动冒烟：10 PASS/2 FAIL，均定位为夹具问题：默认 futex 场景错误选择 writer 事件 `slot_even`；控制 socket 的 `sendall` 被当前执行环境拒绝（EPERM），不是 shm 清理失败。已改为独立继承 pipe 并修正事件选择，没有改组件语义。
- 修正后 debug 完整控制/功能/崩溃矩阵：`python3 test/shm_seqlock_system_test/run_tests.py --binary builddir_system_debug/shm_seqlock_system_worker --matrix --log-dir /tmp/shm-system-debug-matrix`，**616 PASS/0 FAIL**。覆盖 24 核心配置、指定崩溃与恢复窗口、futex 登记/睡眠交错、套圈精确丢失统计。原始记录与每子进程退出码位于该目录。
- 已集成子 agent 图/Heartbeat/资源测试及各自开发记录；资源自移除积累保持失败断言。
- 初版 release 矩阵：616 PASS/0 FAIL（`/tmp/shm-system-release-matrix`）。之后补齐半写 topic/metadata、inline-premark 半写 payload、首次属性/宽松数值解析、全部有向布局切换及无注入黑盒；debug 新增矩阵 744 PASS/0 FAIL（`/tmp/shm-system-debug-expanded`）。
- ASan+UBSan debug 构建成功；`detect_leaks=1` 首轮因当前 ptrace 环境的 LeakSanitizer fatal error 中止，**不算通过**。改用 `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1` 重跑；LeakSanitizer 不可用单独标明，未修改系统配置或第三方。
- 最新主驱动最终矩阵（含生产黑盒、参数失败重试和尾部审计）：debug `/tmp/shm-system-debug-final` 与 release `/tmp/shm-system-release-final` 均 **768 PASS/0 FAIL**。ASan+UBSan（LeakSanitizer 关闭）矩阵 `/tmp/shm-system-asan-nolsan-matrix` **768 PASS/0 FAIL**；之后仅重构 F09 夹具为同组件失败后重试。
- 统一集成工程 `test/shm_seqlock_system_test/suite` 构建 9 目标（两种 component archive 及 7 个独立程序）；`nm -C builddir_suite_release/libproduction_shm_seqlock.a` 无 `systemTestProbe`/`systemTestCopy` 符号，instrumented archive 有预期 `systemTestProbe` 未定义引用。生产/性能路径不含注入。
- F09 新夹具在 ASan+UBSan 下补跑：`--case F09 --matrix` **24 PASS/0 FAIL**（`/tmp/shm-system-asan-parameters`）。
- leader 集成首轮：baseline 4/4 PASS（`/tmp/shm-system-leader-baseline`）；graph 192 PASS、heartbeat PASS、resource 18 PASS/1 FAIL（`/tmp/shm-system-leader-integration`），resource 失败与 DEF-1 一致。
- 2026-11-13 当前 HEAD 统一复跑 `/tmp/shm-system-leader-final2`：core 768 PASS、graph 192 PASS、heartbeat PASS、resource 18 PASS/1 FAIL（DEF-1）、baseline 4/4 PASS；汇总退出码 1 仅由 DEF-1 引起。版本化摘要 [UNIFIED_RESULTS.json](UNIFIED_RESULTS.json)。
- 收尾补跑当前 HEAD 的 ASan+UBSan 完整矩阵 `/tmp/shm-system-asan-current`：**768 PASS/0 FAIL**（`detect_leaks=0`）。此前 `/tmp/shm-system-asan-nolsan-matrix` 为 F09 夹具重构前结果，保留作历史证据。
- 本记录据此更新；正式性能/长跑、推送合并与参考文档同步仍待后续。
- 2026-11-13 用户先确认 DEF-1 采用**方案 A（机会式回收）**并完成实现验证：resource smoke 19/19 PASS；10k 自移除段映射保持 1、总映射 46、RSS 3.9MB（修复前分别为 10,001、30,044、159.6MB）；统一套件 core/graph/heartbeat/resource/baseline 全部 PASS、退出码 0。实验数据见 [DEF1_FIX_EXPERIMENT.json](DEF1_FIX_EXPERIMENT.json)。
- 2026-11-13 用户随后判断该方案对其它代码路径影响较大，指示**回滚自移除修复**。已用 `git restore` 恢复 `src/shm_seqlock/ShmSeqlockComponent.cpp`（该修复从未提交），重新构建后 resource smoke 恢复 18 PASS/1 FAIL（第 18 轮 selfRemove=true），确认 DEF-1 回到基线行为。
- 2026-11-13 用户最终决定 **DEF-1 不修复**：作为已知限制保留失败断言与证据，生产代码不再改动。resource smoke 1 个 FAIL 与统一入口退出码 1 为长期预期状态。
- 2026-11-13 按 PLAN 补齐功能/异常覆盖：F10 查询并发、F11 CPU provider、R03 同段慢/快 callback、R05 并发 add/remove、R06 移除他人/自移除重订阅、R07 最后订阅重加、R08 进程重连（默认 200，已显式跑 1000）、R-dirty-ext 异常头、R-risk 风险探测、R-resource FD/mlock 限制。release 功能矩阵 `--matrix` **778 PASS / 0 FAIL**（`/tmp/shm-func-matrix2`）。
- 2026-11-13 修复夜间重复入口的 O(n²) 写放大：`run_tests.py` 每例增量追加 `results.jsonl`，结束时再写完整 `results.json`；`--repeat=20` 不再每例重写全量 JSON。
- 2026-11-13 夜间 `--repeat=20` 在最终 worker、无并发构建干扰下完成：**15,560 PASS / 0 FAIL**（`/tmp/shm-nightly-repeat20-final`）。此前一轮在 repeat 10 因并行重建 worker 出现 25 个 `PermissionError` FAIL，已作废；该现象是构建与运行冲突，不是测试或产品失败。
- 2026-11-13 debug 功能矩阵复跑：**778 PASS / 0 FAIL**（`/tmp/shm-func-debug-final`）。
- 2026-11-13 R08 显式 1000 次进程重连：**PASS**（`/tmp/shm-r08-1000-final`）。
- 2026-11-13 统一套件复跑 `/tmp/shm-suite-final`：core **778 PASS**、graph **192 PASS**、heartbeat 30 PASS+3 风险探测、resource 18 PASS/1 FAIL（DEF-1）、baseline 4/4 PASS；汇总退出码 1 仅由 DEF-1 引起。
- 2026-11-13 ASan/UBSan 完整矩阵复跑：**778 PASS / 0 FAIL**（`/tmp/shm-func-asan-final2`，`detect_leaks=0`；R-resource 的 fd 上限探测在 sanitizer 下 SKIP，mlock 探测通过）。
- 本轮功能补齐后的最终状态：release/debug/ASan 三套核心矩阵各 778 PASS；夜间重复 15,560 PASS；统一套件除 DEF-1 外全部通过。DEF-1 仍为已知限制，不修复。
- 2026-11-13 用户指示开始性能测试；确认 release/无注入、机器与 NUMA/亲和性，完成 staged 5 轮×30s 基线（26 配置、零丢失）、慢 reader/futex 阈值测量、饱和批次（50 轮、零丢失；0 reader 约 4.5M/s，8 readers 约 1.6–1.7M/s）和 50%/90% 负载（8 配置、40 轮、零丢失）。版本化数据 [PERF_BASELINE.json](PERF_BASELINE.json)、[LOAD_RESULTS.json](LOAD_RESULTS.json)。
- 2026-11-13 用户确认启动全量 1200 配置性能矩阵；按 layout 分 3 块顺序执行（约 12 小时），期间应避免在该机器上运行重负载任务。完成后聚合、更新记录。graph/MultiTransport 性能与 2h/24h 长跑仍待后续。
- 2026-11-13 按要求准备长时间多路稳定性测试：新增 `test/shm_seqlock_stability_test/`（worker + 编排器 + standalone Meson + README），9 个 lane 覆盖固定/变长数据、高低频、多少 reader、writer/reader 随机重启、快慢 reader、布局/block size 等。
- 2026-11-14 07:17 全量 1200 配置性能矩阵完成：3 块各 401 项全部 PASS，240 配置×5 轮，零丢失/损坏/乱序，版本化数据 [PERF_FULL_RESULTS.json](PERF_FULL_RESULTS.json)。
- 2026-11-14 稳定性 worker 构建并冒烟：修复 writer generation 重启复用和 <200µs 速率控制（`sleep_for` 粒度不足，改用自旋）；5 分钟全 lane 冒烟 9/9 PASS，高频 lane 达到 100k/s。
- 2026-11-14 07:30 启动 9 lane 的 72 小时多路稳定性测试 `/tmp/shm-stability-72h`；运行约 1.6 小时后 9/9 lane PASS（汇总已保存）。
- 2026-11-14 09:08 用户要求增加一路超高频变长数据压力测试（`rate=0` 最大吞吐）；新增 `ultra-var-max` lane（变长 64–4096B、8 快 reader、split/stride128/spin），优雅停止 9 lane 批次后于 09:09 以 **10 lane 重新启动** `/tmp/shm-stability-72h-v2`（72 小时，预计 2026-11-17 09:09 完成）。
- 2026-11-14 09:10 首个进度快照：10 lane 无违规、所有 fast reader 零丢失；`ultra-var-max` 最大吞吐约 **84.3k msg/s**（变长 64–4096B，8 reader 全部零丢失），`fixed-high-2r` 达到 100k/s；慢 reader lane 的丢失按预期出现。
- 2026-11-16 编写完整测试报告 [REPORT.md](REPORT.md)（功能、效率、稳定性），所有数据取自本目录结果文件，并单列未覆盖场景；修正 `LOAD_RESULTS.json` 的 `readers` 字段被 per-reader 指标覆盖的问题，聚合脚本固化为 `test/shm_seqlock_resource_system_test/aggregate_load.py`。
- 2026-11-16 跨 NUMA 性能实验完成（`cross_numa_bench.py`，独立进程 taskset 绑核，5 轮 × 35s）：4 种 placement × 1/8 reader × spin/futex，加 4 个饱和配置，共 16 配置、80 轮全部 PASS，lost/dup/ooo/corrupt/bad 全 0。跨节点 spin p50 增加 36–52%，futex 增加 19–23%；饱和吞吐约下降 34%。数据 [PERF_NUMA_RESULTS.json](PERF_NUMA_RESULTS.json)。
- 2026-11-16 1h 跨 NUMA 稳定性首轮作废：两条 lane 复用了同一组 reader 物理核，并发自旋使 p50 放大到约 1.2ms。已改为互不重叠的 4 lane 绑核（4/4/1/4 reader）、新增 `validate_affinity_disjoint` 校验，并把 runner/monitor 固定到控制核后重新启动（`/tmp/shm-stability-numa-1h-v2`）。
- 2026-11-16 1h 跨 NUMA 稳定性完成：**4/4 lane PASS、0 violations**；总发送 252,081,920 条，读者累计接收 900,288,080 条，lost/corrupt/bad/dup/ooo/rejected 全 0；6,001 条资源采样的 ΔRSS/ΔPSS/ΔFD/线程/映射全为 0。数据 [STABILITY_NUMA_RESULTS.json](STABILITY_NUMA_RESULTS.json)。
- 2026-11-16 graph/MultiTransport 性能基准完成：新增 `test/shm_seqlock_graph_bench/`（raw / MultiTransport × 1/4 监听端 × 10k 条/s spin/futex 与饱和，共 12 配置 × 5 轮，5s 预热 + 30s 测量）。**60/60 轮 PASS**；8 个 10k 条/s 配置全部零丢失（p50 3.49–14.9 µs）；饱和时 1 监听端 516,217–591,935 条/s 零丢失，4 监听端因共享读线程出现有损丢弃（219M/273M 条，corrupt/乱序仍为 0）。数据 [GRAPH_PERF_RESULTS.json](GRAPH_PERF_RESULTS.json)，报告见 §5.10。
- 2026-11-16 统一报告单位：新增“0.0 单位约定”，所有 k/M/B 速率明确为条/秒，数据大小用 B/KiB/MiB；相关表头与正文逐一标注。
- 2026-11-16 提升报告可读性：新增第 0 节名词解释（组件与消息、layout/read/wait、spin/futex/churn/inline/spec、指标口径、环境与工具），并在正文首次使用处补充通俗说明；未改动任何实测数字。
- 2026-11-16 09:50 用户确认测试时长已满足，优雅停止 10 lane 稳定性（运行 48.7h）与资源监控，写入最终汇总。结果：**10/10 lane PASS**；总发送 37,666,054,606 条；corrupt/bad/dup/ooo=0；丢失全部来自慢 reader/重启窗口；43 个长期 PID 的 ΔRSS/FD/线程/映射=0、ΔPSS=+6 KiB。数据 [STABILITY_RESULTS.json](STABILITY_RESULTS.json)、[STABILITY_SUMMARY.json](STABILITY_SUMMARY.json)、[STABILITY_RESOURCE_RESULTS.json](STABILITY_RESOURCE_RESULTS.json)。
