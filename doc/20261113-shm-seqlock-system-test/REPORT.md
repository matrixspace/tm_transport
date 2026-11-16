# ShmSeqlockComponent 测试报告（功能 / 效率 / 稳定性）

**报告日期：** 2026-11-16
**被测对象：** `dev::cd606::tm::transport::shm_seqlock::ShmSeqlockComponent`（tm_transport）
**代码基线：** `ccb9a8c`（自 `develop` 创建）
**验证分支：** `feature/shm-seqlock-system-test`
**测试代码版本：** 功能验收 `4d1dd63`；staged 性能 `727f88d`；全量性能矩阵 `4813b53`；稳定性与监控 `134b952`
**测试日期：** 2026-11-13 至 2026-11-16
**数据来源：** 本文所有数字均取自本目录下的结果文件与原始日志，未做推算或补全；各表均标注来源文件。凡未实际执行的项目在“未覆盖场景”中单列。

**给非开发读者的阅读建议：** 先读第 0 节的名词解释；第 4 章回答“功能是否正确”，第 5 章回答“快不快、占多少资源”，第 6 章回答“长时间运行会不会出问题”，第 7 章明确“哪些场景没有测”。表格中的数字均来自实测，未做推算。

---

## 0. 名词解释

本节面向测试、项目管理等非开发读者；熟悉组件的读者可直接跳到第 1 节。

### 0.0 单位约定

- **消息数量用“条”**：k = 1,000 条，M = 1,000,000 条，B = 1,000,000,000 条。本报告中所有“.../s”速率都指**条数/秒**（messages per second），不是字节/秒。
- **数据大小用字节**：B = 字节，KiB = 1,024 字节，MiB = 1,024 KiB。消息大小（如 256B）与内存占用（如 RSS 4,608 KiB）分别标注。
- **时间用 ns / µs / ms / s**：延迟、耗时都是时间；p50/p99 是延迟分位数。

### 0.1 组件与消息

| 名词 | 通俗解释 |
|---|---|
| ShmSeqlockComponent（被测组件） | 同一台机器上的“共享内存广播”：一个发送方把消息写进所有进程都能访问的一块内存，多个接收方各自读取，不走网络。 |
| 发布者 / writer / publisher | 发送消息的一方。 |
| 读者 / reader / subscriber | 接收消息的一方；每个订阅对应一个回调函数。 |
| 段（segment） | `/dev/shm` 下的一块共享内存，通常一个频道（channel）对应一个段。 |
| 槽位（slot）/ 环（ring） | 段内固定大小的“格子”，首尾相接成环；发布者写完一圈会覆盖最旧的格子。 |
| 套圈（lap）/ lapResync | 读者太慢、落下一整圈以上时，旧消息已被覆盖，读者只能跳到最新位置继续读；跳过的条数计入“丢失”。 |
| topic / payload | 消息主题（可用于过滤）/ 消息正文。 |
| 订阅 / 退订 | 读者登记 / 注销一个接收回调。 |
| 自移除（self-remove） | 回调在自己所在的读取线程里把自己退订（例如“收到一条就退订”）。 |
| 回调（callback） | 收到消息后执行的用户函数。 |
| churn | 反复“订阅 → 收消息 → 退订”或反复断开/重连，用来看资源是否越积越多。 |
| slotCount / slotSize | 环里有多少个槽位 / 每个槽位能放多少字节（topic + payload）。 |
| writerLock | 发布端的互斥锁：多个线程通过同一个发布者写消息时串行化。 |
| mlock | 把共享内存锁在物理内存里，避免被换出。 |
| stride128 | 槽位按 128 字节对齐（跨 CPU 缓存行），减少读写互相干扰。 |

### 0.2 三个可配置项（本报告表格中的 layout / read / wait）

| 配置 | 取值 | 通俗解释 |
|---|---|---|
| 布局 layout | inline | 消息正文直接放在槽位里，写完一次性提交。 |
| | split | 把“序号/长度”和“数据正文”放在不同缓存行，减少读写干扰。 |
| | inline-premark | inline 的优化写法：先把下一个槽位标记为“正在写”，再把数据一次写入。 |
| 读方式 read | classic | 常规读：先看序号，再读数据，最后确认序号没变。 |
| | spec | 推测读：不等序号，先按上一条的长度预读，最后再校验；某些负载下更快，某些更慢（报告中有对比）。 |
| 等待方式 wait | spin | 读者空闲时“忙等”：不停循环检查有没有新消息，占满一个 CPU 核，延迟最低。 |
| | futex | 读者空闲时睡眠，发布者写完消息后唤醒它；省 CPU，但唤醒本身带来额外延迟。 |

说明：spin（自旋）和 futex（Linux 的睡眠/唤醒机制）是两种“等消息”的方式。本报告的延迟表里，futex 的延迟明显高于 spin，就是睡眠/唤醒的代价；而 futex 的 CPU 占用低得多。

### 0.3 指标

| 名词 | 含义 |
|---|---|
| offered/s | 发布者每秒尝试发送的条数（条/秒，目标发送速率）。 |
| accepted / received | 成功交给共享内存 / 读者实际收到。 |
| lost | 因读者太慢、数据被覆盖而跳过的条数（有损广播的正常现象）。 |
| rejected | 因消息超过槽位容量而被拒绝的条数。 |
| publish 耗时 | 发布函数本身执行了多久（不含读者）。 |
| 端到端延迟 | 从发布者发出到读者回调收到的时间。 |
| p50 / p90 / p99 / max | 延迟分位数：p50 是一半样本低于它（“典型值”），p99 是 99% 样本低于它（“尾部”），max 是最慢的一条。 |
| 丢失率 | lost / offered。 |
| CPU% | 进程占用的处理器比例；100% ≈ 占满一个核。 |
| RSS / PSS | 进程实际占用的内存；PSS 按共享页分摊，更接近真实占用。 |
| FD | 文件描述符（打开的文件、管道、socket、共享内存句柄的总数）。 |
| 预热 / 测量窗口 | 正式开始统计前先跑一段（如 5 s）并丢弃，之后的数据才计入结果。 |
| 饱和 | 不设发送速率上限，让发布者全速发送，用来看系统最多能跑多快。 |
| 50% / 90% 负载 | 以该配置的饱和速率为 100%，分别按一半、九成速率发送。 |

### 0.4 环境与工具

| 名词 | 含义 |
|---|---|
| CPU 亲和性 / 绑核 / taskset | 把进程固定到指定 CPU 核上，减少跨核迁移带来的抖动。 |
| 物理核 | CPU 的实体核心；每个核有 2 个超线程。绑核时使用物理核，避免两个超线程互相争抢。 |
| NUMA0 / NUMA1 | 服务器的两个“本地内存区”；CPU 访问本区内存快，访问另一区慢。本机 NUMA0 是偶数编号 CPU，NUMA1 是奇数编号 CPU。 |
| 跨 NUMA | 发布者在 NUMA0、读者在 NUMA1（或反向），读者需跨区读取共享内存，延迟更高。 |
| 进程重连 / 重启 | 杀掉读者或发布者进程再重新启动，验证恢复能力。 |
| 故障注入（probe） | 只在测试版本里加入的“暂停点”，可在写内存的精确瞬间杀掉进程；正式版本不包含这些代码。 |
| 黑盒 | 不带任何注入，在随机时刻杀进程，验证真实使用下的恢复。 |
| ASan / UBSan / TSan / LeakSanitizer | 开发期检查工具：内存越界、未定义行为、线程竞争、内存泄漏。 |
| 图（Graph）/ RealTimeApp | 框架的上层用法：把多个处理节点连成数据流图，本组件作为图里的输入/输出节点。 |
| MultiTransport | 框架的统一传输接口，同一套上层代码可切换不同传输实现。 |
| Heartbeat / 发现 | 服务心跳与发现：服务定期广播自己的地址，客户端据此找到并订阅业务数据。 |
| fetch-first | “先取一条最新消息，然后自动退订”的一次性读取用法。 |
| hook | 收发前后的数据转换钩子（如编码/解码）。 |
| DEF-1 | 本报告唯一保留的已知缺陷编号，详见 §4.6。 |
| R08 等编号 | 测试用例编号，含义见对应章节和测试入口说明。 |
| lane | 稳定性实验里的一路独立场景（一个发布者 + 若干读者 + 自己的参数）。 |

---

## 1. 结论摘要

下表是结论速览；不熟悉术语的读者请先看第 0 节。

| 维度 | 结论 |
|---|---|
| 功能正确性 | 通过。核心矩阵在 release / debug / ASan+UBSan 下均 778/778 PASS；夜间 20 次全窗口重复 15,560/15,560 PASS；图集成 192/192；真实 Heartbeat 30 PASS；既有基线 4/4。 |
| 已知缺陷 | 1 项：**DEF-1**——callback 在自身 reader 线程内移除最后一条订阅后，订阅对象与段映射保留到组件析构。经用户确认**不修复**，作为已知限制；测试保留失败断言（资源 smoke 18/19），统一入口退出码 1 为长期预期。 |
| 效率 | 通过。全量 240 配置 × 5 轮 = 1200 轮、10k 条/s（offered/s，目标发送速率），**零丢失/零损坏/零乱序/零拒绝**；1 reader 256B spin publish 均值 105 ns、端到端 p99 902 ns；饱和吞吐 0 reader 4.5M 条/s、8 readers 1.6–1.7M 条/s；50%/90% 负载 8 配置零丢失；慢 reader 下 writer 成本远低于阈值；futex 空闲 reader CPU 0.0–0.2%（阈值 <5%）。跨 NUMA 补充实验（§5.9）中跨节点使 spin 端到端 p50 增加 36–52%、饱和吞吐下降约 34%。图/MultiTransport 路径（§5.10）在 10k 条/s、1/4 监听端、spin/futex 下均零丢失；饱和时 1 监听端约 0.52–0.59M 条/s，4 监听端因共享读线程出现有损丢弃（按设计）。 |
| 稳定性 | 通过。10 lane 多路并发运行 **48.7 小时**（原计划 72h，用户确认时长足够后停止）：10/10 PASS，总发送 37.67B 条，corrupt/bad/dup/ooo 全 0，writer 重启 828 次 / reader 重启 1147 次全部恢复；272,633 条资源采样显示长期进程 **ΔRSS/ΔFD/Δ线程/Δ映射 = 0**。跨 NUMA 补充 1h 实验（§6.5）：4/4 lane PASS、零丢失、资源零增长。 |
| 覆盖边界 | **有明确未覆盖项**，见第 7 节。本报告不构成“所有场景已验证安全”的结论；未观察到的风险不等于不存在。 |

**关键限制（先读）**：性能数据来自共享主机、未独占；CPU 频率调节 sysfs 在本机不可用，无法固定频率；分位数是“每轮分位数的均值”，不是合并样本分位数；未报告 p99.9；LeakSanitizer 在当前 ptrace 环境不可用，ASan 只能 `detect_leaks=0`；TSan 未运行。

---

## 2. 被测组件与测试范围

ShmSeqlockComponent 是同机单发布者多读者（SPMC）共享内存广播组件：段位于 `/dev/shm`，由定长 slot 组成的环构成，每个 slot 由自身序号保护（seqlock）。发布者从不等待读者，落后超过 `slotCount` 条的读者会被重新同步到最新消息并计为丢失。

测试覆盖：组件直接调用（非图）、RealTimeApp 数据流图、MultiTransport、typed/hook、fetch、真实 Heartbeat 与发现、故障注入（仅测试构建）、无注入黑盒、资源与性能、多路长时间稳定性。生产构建不包含任何故障注入代码（`nm -C libproduction_shm_seqlock.a` 无 `systemTestProbe`/`systemTestCopy` 符号）。

---

## 3. 测试环境与构建

| 项目 | 值 |
|---|---|
| 机器 | Intel(R) Xeon(R) Gold 5117 CPU @ 2.00GHz；2 socket × 14 core × 2 HT = 56 逻辑 CPU；NUMA 0–1 |
| 内核 | Linux 4.18.0-372.9.1.el8.x86_64（Rocky Linux 8.6） |
| 编译器 / 构建 | GCC 15.2.1；Meson 1.11.2；C++17 |
| 构建类型 | release（性能/稳定性）、debug、ASan+UBSan（`-Db_sanitize=address,undefined`，`detect_leaks=0`） |
| 频率控制 | `scaling_governor` / `scaling_cur_freq` sysfs 不存在，**未固定频率** |
| 亲和性 | 默认 0–55；性能批次 pin 到 NUMA0 的 10 个物理核 `16,18,20,22,24,26,28,30,32,34`；稳定性批次未绑核 |
| 主机占用 | 共享主机，**未独占**；测试期间其他任务可能同时运行 |

验证构建统一使用独立 Meson 工程（`test/shm_seqlock_system_test/suite`、各子目录 `standalone`），不复用其它工作区缓存，未修改 vcpkg/第三方库。

---

## 4. 功能测试

### 4.1 测试方法

- 每个用例分配唯一段名，登记并清理全部子进程、段与锁文件；控制面使用独立 pipe/stdio，不复用被测数据通道。
- 就绪判定区分“进程启动”“订阅 API 返回”“reader 已映射并确定起点”，不使用固定 sleep 代替。
- 数据包含任意 NUL 与空字节；oracle 比较完整 topic/payload 原始字节，不依赖组件的 `corrupt` 计数。
- 故障注入点仅在测试构建启用；生产构建另跑无注入黑盒随机 SIGKILL。
- 超时由外部 watchdog 处理，保存线程栈/状态/wchan/maps 后再清理。

### 4.2 核心功能矩阵

矩阵由 24 组线格式组合（3 layout × 2 read × 4 writer/reader wait）× 32 类用例作业 = 768 项，加上 10 个生命周期/异常用例各跑一次，共 **778 项**。layout（内存布局）、read（读取方式）、wait（空闲等待方式）是组件的三个可配置项，通俗解释见第 0.2 节；24 组就是它们的全部搭配。

| 构建 | 命令 | 结果 | 证据 |
|---|---|---|---|
| release | `run_tests.py --matrix` | **778 PASS / 0 FAIL** | `/tmp/shm-func-matrix2`；统一套件 `/tmp/shm-suite-final/core` |
| debug | 同上 | **778 PASS / 0 FAIL** | `/tmp/shm-func-debug-final` |
| ASan+UBSan（debug，`detect_leaks=0`） | 同上 | **778 PASS / 0 FAIL** | `/tmp/shm-func-asan-final2`（R-resource 的 fd 上限探测在 sanitizer 下 SKIP） |
| 夜间重复 | `--matrix --repeat=20` | **15,560 PASS / 0 FAIL** | `/tmp/shm-nightly-repeat20-final` |

覆盖的用例类别（详见 [测试入口说明](../../test/shm_seqlock_system_test/README.md)）：

| 类别 | 内容 |
|---|---|
| C00 | 确定性错误 topic/payload、逆序、重复必须被 oracle 检出 |
| F01–F02 | reader 先/后启动、多进程 reader、空/二进制 topic 与 payload、缓存行与 320B 预测边界、恰满 |
| F03 | 反复超一拒绝后合法发布；publisher rejected 与 reader received 精确对账 |
| F04–F05 | 无过滤/精确/regex 过滤、hook 往返/nullopt、hook 扩展后超限 |
| F07 | latest/next、静止活 writer、死 writer 残留与接管 |
| F08 | 首次 sender 属性、读端遵循段头、同进程不同组件锁竞争 |
| F09 | 非法名字/数值/layout、失败后同组件可重试、宽松解析记录 |
| F10 | 4 线程并发查询 stats/threadHandles，同时 100 次 add/publish/remove；结束无残留 reader |
| F11 | CPU lease provider：缺失报错、成功传播 who/spec、重复安装抛异常、provider 抛异常后可回退 |
| R01 | callback 屏障暂停，精确推进 N−1/N/N+1/2N+1，按 layout 对账套圈与 lost |
| R03 | 同段慢/快 callback 共享 reader 线程（无错误隔离） |
| R04-crash | 14 个精确崩溃窗口（持锁/创建/magic、odd、半写 metadata/topic/payload、even/published/premark、superseded、恢复扫描/epoch）后 SIGKILL 并接管 |
| R04-transition | 更换 layout/count/capacity/stride，覆盖全部有向 layout 切换 |
| R05 | 4 线程并发 add、同时 remove（含重复/未知 id）20 轮 |
| R06 | callback 移除其他订阅；自移除后从 reader 线程重新订阅 |
| R07 | 最后订阅退出后以不同 layout/read/wait 重新添加；重复/未知 remove 安全 |
| R08 | 进程重连 churn：默认 200 次；显式 1000 次另测 PASS |
| R-futex | futex 登记/睡眠前后与 publish 交错 |
| R-waiter-leak | kill 已登记 reader，泄漏 waiter 经连续空唤醒清理 |
| R-writer-conflict | 8 竞争者明确失败、SIGSTOP 仍持锁、SIGKILL 后可接管 |
| R-blackbox | 无注入 worker 按固定 seed 在发送时随机 SIGKILL，精确检查已完成区间后接管 |
| R-dirty | 零长度/短头/零 magic/无效头，reader 先启动，新 writer 重建后正常 |
| R-dirty-ext | 结构合法但字段异常：odd seq、超长长度、错误 topicHash、published 越过环 |
| R-resource | `RLIMIT_NOFILE=5` 下 writer 明确失败（"Too many open files"）；`RLIMIT_MEMLOCK=0` 下 mlock 失败但读写正常 |

### 4.3 图、MultiTransport 与 fetch

16 个场景 × 3 layout × 2 read × 2 同 wait 组合 = **192 项，192 PASS / 0 FAIL**（release，`/tmp/shm-suite-final/graph`）。

场景：raw_graph、typed_graph、typed_explicit_hooks、multitransport、concurrent、threaded_exporter、shared_exporter、control、slow_sync、slow_async、fetch_raw、fetch_typed、multi_fetch_raw、multi_fetch_typed、synchronous、dynamic_listener。覆盖 G02/G03/G04/G05/G06/G07、G08 fetch、G09 慢同步/异步下游，以及 F05/F06（typed 非法编码、错误类型、截断，解码失败不进入业务 callback）。

`control` 场景另做 12 参数 × 5 次重复、每次 20 个停止/重启循环，**60/60 PASS（共 1200 个循环）**（debug，`/tmp/shm-graph-debug-control-repeat/results.json`）。

### 4.4 真实 Heartbeat 与发现

11 个场景 × 3 layout = 33 项结果：**30 PASS**，3 项为 H04 风险探测（不计入 PASS）。

| 场景 | 覆盖 |
|---|---|
| direct | 非图显式发布心跳；UUID/PID/服务名/时间/状态/channel 列表 |
| timer / timer_separate | 图定时发布；数据与心跳共段/独立段 |
| shared | Heartbeat/alert/custom 共段，topic/类型隔离，首次启用 writerLock |
| growth | 心跳恰满/超限，状态与 channel 列表增长 |
| restart | 服务重启、地址更新后的发现与订阅 |
| address | 发现后订阅实际业务广播 |
| lock | 锁竞争 |
| ttl | TTL 过期与恢复；死 writer 残留不让新客户端误上线 |
| slow_reader | 慢 reader 对心跳/发现的影响 |
| risk_first_unlocked | 首次未加锁、后续心跳 locator 才加锁：不能补救已有 sender |

**H04 风险探测结果**：最终记录的 release 轮次中，inline-premark 为 `RISK_OBSERVED`，inline 与 split 为 `RISK_NOT_OBSERVED`；更早的完整 release 轮次曾在 inline 观察到异常，追加证据轮在 split 与 inline-premark 观察到异常。跨轮次三个 layout 均至少出现过一次，属随机时序，“未观察到”不表示安全。证据：[heartbeat-release-results.json](heartbeat-release-results.json)、[heartbeat-risk-results.json](heartbeat-risk-results.json)。

### 4.5 资源与回归基线

| 套件 | 结果 | 证据 |
|---|---|---|
| resource smoke（oracle + 12 组 layout/read/wait 内容校验 + 0/2/4/8 reader + 2 组 churn） | **18 PASS / 1 FAIL**；oracle 对 128 个确定性逐字节损坏全部检出 | `/tmp/shm-suite-final/resource` |
| 既有 shm_seqlock_test（inline / split / inline-premark） | **3/3 PASS**（每 layout 24–26 项检查） | `/tmp/shm-suite-final/baseline-*` |
| 既有 ipc_concurrency_test（shm_seqlock） | **PASS** | `/tmp/shm-suite-final/baseline-concurrency.log` |
| 10,000 次普通 churn（反复“订阅→收消息→退订”，由主线程退订） | PASS：段映射 1 → 1 → 1 | [RESOURCE_RESULTS.json](RESOURCE_RESULTS.json) |
| 10,000 次自移除 churn（回调在读取线程里退订自己） | **FAIL**：段映射 1 → 10,001 → 1（析构后释放） | 同上 |

### 4.6 已知缺陷 DEF-1

**现象**：callback 在自身 reader 线程内调用 `shm_seqlock_removeSubscriptionClient()` 移除该段最后一条订阅后，组件仍存活期间每次自移除都保留一份段映射和已退出但未 join 的线程对象；只有组件析构才统一释放。

**证据**（release，`churn --cycles=10000 --wait=futex --selfRemove=true`）：

| 指标 | 基线 | 组件存活（10k 次后） | 析构后 |
|---|---:|---:|---:|
| 段映射数（含 writer） | 1 | **10,001** | 1 |
| 总映射数 | 42 | **30,044** | 52 |
| RSS KiB | 2,124 | **159,628** | 39,684 |
| FD / 线程数 | 5 / 1 | 5 / 1 | 5 / 1 |

普通（非自移除）路径对照始终为 1 个段映射。数据面正确性、FD、线程数不受影响；风险是长生命周期组件 + 大量自移除订阅时映射与 RSS 线性增长（本机 `vm.max_map_count=65530`）。根因位于 `src/shm_seqlock/ShmSeqlockComponent.cpp` 的 `stoppedSubscriptions_`：reader 线程内停止的订阅被移入该容器，仅组件析构时清空。

**处理决定（2026-11-13）**：曾实现并验证“机会式回收”方案（resource smoke 19/19；10k 自移除段映射保持 1），但用户评估其对其它代码路径影响较大，要求回滚；随后决定 **不修复**，作为已知限制。测试保留严格断言与证据，不豁免该失败；实验记录见 [DEF1_FIX_EXPERIMENT.json](DEF1_FIX_EXPERIMENT.json)（`reverted: true`）。

### 4.7 风险探测（不支持用法，不构成安全保证）

| 探测 | 观察结果 |
|---|---|
| callback 抛异常 | reader 进程 SIGABRT（-6），组件不捕获用户异常 |
| publisher 捕获函数在组件析构后调用 | 进程 SIGSEGV（-11） |
| 活跃映射被外部 truncate | writer 与 reader 均死亡（SIGBUS） |
| writer 持锁期间替换 `.lock` 文件 | 第二个 writer 可以成功取得段 |
| 伪造 `published = 2^63` | `2*next+2` 回绕，reader 误读 slot 0 并交付 1 条消息 |

以上均为“观察到的行为”，不是契约，也不代表该用法安全。

---

## 5. 效率测试

### 5.1 方法与口径

- 构建：release，无故障注入（`--mode=environment` 确认 `build_type=release`、`fault_probes=false`）。
- 每配置 5 轮 ×（5 s 预热 + 30 s 测量），按固定 seed 随机顺序；10,000 offered/s（饱和轮次 `rate=0`）。
- 每 reader 每轮 300,000 样本；延迟分位数取“每轮分位数的均值”，**不是合并样本分位数**；未报告 p99.9。
- 发送按单调时钟开放式进行，不用 reader ACK 控速；性能轮次只校验头、topic、序号，内容逐字节由功能轮次保证。
- 性能批次 pin 到 NUMA0 的 10 个物理核；主机共享、未独占；频率不可固定。

### 5.2 全量配置矩阵

240 个唯一配置 × 5 轮 = **1,200 轮**，覆盖 readers 0/1/2/4/8 × payload 64/256/1024/4096B × layout inline/split/inline-premark × read classic/spec × wait spin/futex，10k offered/s。这里一个“配置”就是一组参数选择：几个读者、消息多大、用哪种布局/读法/等待方式；10k 条/s（offered/s）表示发布者按每秒 1 万条的目标速率发送。

| 指标 | 结果 |
|---|---|
| 轮次 | 1200，全部 PASS |
| reader 累计接收样本 | 1,080,000,000 条 |
| lost / bad / order errors / rejected | **0 / 0 / 0 / 0** |
| publish 均值范围 | **84.5 ns – 9.29 µs** |
| publish p99 范围 | **111.4 ns – 11.6 µs** |
| 单 reader p99 均值最高 | **13.85 µs**（8 readers、64B、inline-premark、classic、futex） |

来源：[PERF_FULL_RESULTS.json](PERF_FULL_RESULTS.json)；原始 `/tmp/shm-perf-full-{inline,split,inline-premark}/`。

### 5.3 代表性结果（10k 条/s，5 轮均值）

表中 R0/R1/R2/R4/R8 表示读者数量；“publish”是发布函数自身的耗时，“端到端”是从发布者发出到读者回调收到的时间，两者都是越小越好（名词见第 0.3 节）。

**reader 扩展**（payload 256B，inline，classic）：

| readers | spin publish 均值 / p99 | spin 端到端 p50 / p99 | futex publish 均值 / p99 | futex 端到端 p50 / p99 |
|---:|---:|---:|---:|---:|
| 0 | 96.0 ns / 160 ns | — | 111.8 ns / 186 ns | — |
| 1 | 105 ns / 252 ns | 462 ns / 902 ns | 2.35 µs / 2.72 µs | 4.87 µs / 5.64 µs |
| 2 | 102 ns / 294 ns | 571 ns / 778 ns | 3.22 µs / 3.55 µs | 5.20 µs / 6.05 µs |
| 4 | 111 ns / 342 ns | 638 ns / 891 ns | 5.07 µs / 5.54 µs | 6.62 µs / 8.19 µs |
| 8 | 109 ns / 378 ns | 754 ns / 1.13 µs | 8.29 µs / 9.37 µs | 7.08 µs / 10.83 µs |

来源：[PERF_BASELINE.json](PERF_BASELINE.json)（staged 批次，每配置 5 轮）。进程 CPU：0 reader 约 100%，1 reader 约 200%，8 readers spin 约 898%。

**payload 扩展**（1 reader，inline，classic）：

| payload | spin publish 均值 | futex publish 均值 | spin 端到端 p50 / p99 |
|---:|---:|---:|---:|
| 64B | 92.6 ns | 2.30 µs | 349 ns / 505 ns |
| 256B | 105 ns | 2.35 µs | 462 ns / 902 ns |
| 1024B | 144 ns | 2.37 µs | 741 ns / 1.01 µs |
| 4096B | 657 ns | 2.84 µs | 1.67 µs / 2.16 µs |

**layout / read**（1 reader，256B，spin）：

| 配置 | publish 均值 | 端到端 p50 / p99 |
|---|---:|---:|
| inline / classic | 105 ns | 462 ns / 902 ns |
| inline / spec | 95.7 ns | 399 ns / 709 ns |
| split / classic | 101 ns | 446 ns / 637 ns |
| split / spec | 103 ns | 407 ns / 765 ns |
| inline-premark / classic | 108 ns | 404 ns / 584 ns |
| inline-premark / spec | 104 ns | 381 ns / 1.40 µs |

### 5.4 饱和吞吐

`rate=0` 表示不设目标速率、发布者全速发送（即“饱和”），5 轮 × 30 s，payload 256B，inline，classic：

| readers | spin 实测发送速率（条/秒，最小–最大） | futex 实测发送速率（条/秒，最小–最大） |
|---:|---|---|
| 0 | 4.49M – 4.57M | 3.95M – 4.04M |
| 1 | 3.74M – 3.82M | 2.69M – 2.71M |
| 2 | 2.73M – 3.05M | 2.27M – 2.38M |
| 4 | 2.12M – 2.30M | 1.95M – 2.18M |
| 8 | 1.66M – 1.73M | 1.58M – 1.65M |

除 1 reader spin 在 5 轮合计 567.35M 条中丢失 8,191 条（**0.0014%**）外，其余饱和配置 lost=0。该结果说明在这些配置下 writer 先达到自身吞吐上限，而不是 reader 跟不上。来源：[LOAD_RESULTS.json](LOAD_RESULTS.json)。

### 5.5 50% / 90% 负载

以该配置“全速”时的最小实测速率为 100%，分别按 50% 和 90% 两档发送；每项 5 轮 × 30 s。**8 个配置全部零丢失**：

| 配置 | 请求速率（条/秒） | 实际发送（条/秒） | loss | reader p50 / p99 |
|---|---:|---:|---:|---:|
| 1r spin 50% | 1,869,965 | 1,869,960 | 0 | 472 ns / 552 ns |
| 1r spin 90% | 3,365,937 | 3,265,100 | 0 | 375 ns / 765 ns |
| 1r futex 50% | 1,342,680 | 1,342,680 | 0 | 489 ns / 573 ns |
| 1r futex 90% | 2,416,824 | 2,333,480 | 0 | 473 ns / 584 ns |
| 8r spin 50% | 831,020 | 831,020 | 0 | 714 ns / 875 ns |
| 8r spin 90% | 1,495,836 | 1,495,840 | 0 | 738 ns / 1.12 µs |
| 8r futex 50% | 787,670 | 787,670 | 0 | 723 ns / 896 ns |
| 8r futex 90% | 1,417,806 | 1,417,810 | 0 | 730 ns / 1.02 µs |

90% 轮的 1 reader 实际速率略低于请求（spin 3.27M vs 3.37M，futex 2.33M vs 2.42M），说明已接近 writer 上限，但 reader 仍无丢失。

### 5.6 慢 reader 成本与 futex 空闲 CPU

来自既有 `shm_seqlock_test`（release，绑到同一组 CPU 核）。这里检查“存在很慢的读者时，发布者会不会被拖慢”；futex 空闲 CPU 指读者没有消息可收时占用的处理器比例。

| layout | writer 平均耗时（基线 → 有慢 reader） | 阈值 1.5×base+50ns | 结论 |
|---|---:|---:|---|
| inline | 142.5 → 138.8 ns | 263.8 ns | 通过 |
| split | 146.1 → 147.0 ns | 269.2 ns | 通过 |
| inline-premark | 140.1 → 147.2 ns | 260.2 ns | 通过 |

慢 reader 在该轮收到 1,626–5,642 条、丢失约 19.4–19.8 万条（有损广播按设计），两个 fast reader 各收 200,000 条、零丢失。

futex 空闲 reader CPU：inline 0.0%、split 0.2%、inline-premark 0.0%（阈值 <5%）；futex reader + spin publisher 对照 0.8%；busy-spin 对照约 100%。

inline-premark 的 `testNoPremark` spec 负对照本轮未观察到撕裂（torn=0），按方案记为“不充分”，不是硬失败；同族负对照在早期功能运行中曾观察到 torn=152。

### 5.7 资源占用（性能轮次）

- 每配置测量前后 RSS/PSS/FD/线程/映射稳定；spawn 后不再增长的进程无映射积累。
- 10k 条/s、1 reader、256B：RSS 约 6,000–6,300 KiB（约 5.9–6.2 MiB），FD 5，线程 2，段映射 2。
- 大规模场景：1 MiB payload（2 MiB slot × 64）reader/writer RSS 约 136,028 / 135,676 KiB（约 133 MiB），稳定（稳定性测试 48.7 h 内 RSS 无增长）。

### 5.8 效率结论与限制

在上述配置下，组件满足 PLAN 设定的性能门槛：慢 reader 不增加 writer 成本、futex 空闲 CPU 远低于 5%、10k 条/s 与 50%/90% 负载零丢失。**但**：

- 只覆盖到 8 readers、4 KiB 常规 payload（1 MiB 只在稳定性低频验证）；
- 只有一批 5 轮测量，未做同机两批稳定性复测，不能据此判定性能回归；
- 分位数口径为每轮均值，样本 30 万/reader/轮，无 p99.9；
- 主机共享、频率不可固定，绝对数值不可跨机直接比较。

### 5.9 跨 NUMA 节点性能（2026-11-16 补充）

独立进程基准（`test/shm_seqlock_stability_test/cross_numa_bench.py`）：每轮 1 个 writer 进程 + N 个 reader 进程，各自 `taskset` 绑定到单个**物理核**（一个核只跑一个进程）；5 s 预热 + 30 s 测量，5 轮，10,000 offered/s，payload 256B，inline/classic。NUMA0/NUMA1 是服务器的两个“本地内存区”：CPU 访问本区内存快，访问另一区慢。`same0`/`same1` 为同节点对照，`cross01` 为 writer 在 NUMA0、reader 在 NUMA1（共享内存页由 writer 在 NUMA0 首次写入，因此落在 NUMA0），`cross10` 为反向。16 个配置 × 5 轮 = **80 轮全部 PASS**；lost / dup / ooo / corrupt / bad / rejected 全为 0。数据 [PERF_NUMA_RESULTS.json](PERF_NUMA_RESULTS.json)。

| 配置 | 同节点 p50 / p99 | 跨节点 p50 / p99 | p50 变化 | p99 变化 |
|---|---:|---:|---:|---:|
| 1 reader spin | 2.04 / 2.33 µs | 2.78 / 3.62 µs | +36% | +56% |
| 1 reader futex | 10.75 / 12.85 µs | 12.77 / 15.16 µs | +19% | +18% |
| 8 readers spin | 2.40 / 2.70 µs | 3.65 / 4.57 µs | +52% | +69% |
| 8 readers futex | 16.15 / 20.58 µs | 19.15 / 24.37 µs | +19% | +18% |
| 8 readers spin（反向） | 2.51 / 2.91 µs | 3.74 / 4.67 µs | +49% | +60% |
| 8 readers futex（反向） | 16.69 / 23.14 µs | 20.48 / 24.53 µs | +23% | +6% |

`rate=0` 饱和（8 readers，spin）：NUMA0 同节点 587,604 条/s，跨节点 387,800 条/s（**−34.0%**）；NUMA1 同节点 586,693 条/s，反向跨节点 390,439 条/s（**−33.5%**）。跨节点时 reader 的每次槽位读取都走远端内存，spin 模式的延迟与吞吐损失最明显；futex 模式本身以唤醒开销为主，跨节点只增加约 18–23% 的延迟。

**口径说明（重要）**：这些进程独立运行、reader 做逐字节 CRC 校验且 `spinCount=1`，绝对延迟高于第 5.2–5.3 节的进程内资源测试（如 1 reader 256B spin 的 462 ns），**两组数字不可直接混用**；本节的同节点/跨节点比值才是结论。基准运行时 `git status` 仅显示本次新增/修改的测试脚本，生产组件源码未改动（见 `environment.dirty_status`）。

**限制**：只覆盖 2 个 NUMA 节点上的选定物理核；未测试内存 interleave、`numactl --membind` 策略，也未覆盖 graph/MultiTransport 路径的跨节点行为。

### 5.10 图与 MultiTransport 性能（2026-11-16 补充）

使用 `test/shm_seqlock_graph_bench/` 测量真实 `RealTimeApp` 图路径：触发导入器 → 发布节点 → 共享内存 → 导入器/监听节点 → 接收节点（同一进程内回环）。`raw` 直接使用 `ShmSeqlockImporterExporter`，`multi` 使用 MultiTransport 的 shm_seqlock 通道；消息为 256 字节正文 + 40 字节测试头，接收节点逐字节校验并统计“发送→回调”延迟。每配置 5 轮 ×（5 s 预热 + 30 s 测量）。12 配置、60 轮全部 PASS，dup/ooo/corrupt 全 0。数据 [GRAPH_PERF_RESULTS.json](GRAPH_PERF_RESULTS.json)。

**10k 条/s（全部零丢失）**：

| 模式 | 监听节点 | 等待 | 实测发送（条/秒） | 丢失（条） | p50 | p90 | p99 |
|---|---|---:|---:|---:|---:|---:|---:|
| raw | 1 | spin | 10,000 | 0 | 3.69 µs | 4.08 µs | 4.45 µs |
| raw | 1 | futex | 10,000 | 0 | 11.9 µs | 12.6 µs | 15.0 µs |
| raw | 4 | spin | 10,000 | 0 | 3.49 µs | 3.95 µs | 4.34 µs |
| raw | 4 | futex | 10,000 | 0 | 13.7 µs | 14.4 µs | 16.3 µs |
| multi | 1 | spin | 10,000 | 0 | 3.91 µs | 4.34 µs | 4.72 µs |
| multi | 1 | futex | 10,000 | 0 | 14.1 µs | 15.5 µs | 16.5 µs |
| multi | 4 | spin | 10,000 | 0 | 4.01 µs | 4.39 µs | 4.74 µs |
| multi | 4 | futex | 10,000 | 0 | 14.9 µs | 16.3 µs | 18.2 µs |

延迟按监听节点 0 统计（每轮直方图分位数的均值）；4 监听端时后续节点的延迟略高，因为同一段的回调在一个读线程上顺序执行。

**饱和（rate=0，spin）**：

| 模式 | 监听节点 | 实测发送（条/秒） | 丢失（条） | p50 | p99 |
|---|---|---:|---:|---:|---:|
| raw | 1 | 516,217 | 0 | 3.41 µs | 4.68 µs |
| raw | 4 | 699,615 | 219,004,632 | 6.37 ms | 12.5 ms |
| multi | 1 | 591,935 | 0 | 3.19 µs | 659 µs（p99 最大 3.28 ms） |
| multi | 4 | 716,722 | 272,769,240 | 6.07 ms | 11.8 ms |

**解读**：4 监听端饱和时出现大量“丢失”不是数据损坏，而是有损广播的设计行为——每条消息要依次送给同一段的 4 个回调，它们共享一个读线程；全速发送时消费速度跟不上，环被覆盖后读者跳到最新位置（丢失计数增加），而 corrupt/乱序仍为 0。1 监听端在约 0.5–0.6M 条/s 的饱和速率下仍能全部接收。

MultiTransport 与 raw 在 10k 条/s 下的延迟差异约 0.1–1.2 µs，反映管理层开销；`multi` 1 监听端饱和时的 p99 尾部（659 µs）明显高于 raw（4.68 µs），属于调度相关的观察，只有一批 5 轮数据，不作为回归结论。

**口径**：图路径的绝对延迟包含触发节点调度与逐字节校验，高于 §5.2–5.3 的进程内资源测试（如 1 reader 256B spin 的 462 ns），**不可混用**；本节只做 raw/multi、1/4 监听端的相对比较。基准运行时的 `git status` 仅包含本次新增的测试程序和文档改动，生产组件源码未修改。

---

## 6. 稳定性测试

### 6.1 方法

自驱动 worker + Python 多路编排器（`test/shm_seqlock_stability_test/`），生产组件、无注入。每个 lane（实验通道）是一路独立场景：1 个发布者进程 + 若干读者进程，有自己的消息大小、发送速率和内存布局。writer 按设定速率发送带 generation、序号、发送时间、长度与 FNV-1a 校验和的报文；reader 校验完整性、顺序，统计丢失/重复/乱序/损坏/延迟；慢 reader 在 callback 中 sleep。Python 负责多 lane 并发、随机 SIGKILL 重启 writer/reader、资源采样与汇总。

10 个 lane：

| lane | 数据 | 发送速率（条/秒） | reader | 布局/参数 | 重启 |
|---|---|---:|---|---|---|
| fixed-low-2r | 固定 64B | 100 条/s | 2 快 | inline 512/4096 futex | 无 |
| fixed-high-2r | 固定 64B | 100k 条/s | 2 快 | inline 512/4096 spin | 无 |
| var-high-8r | 变长 64–4096B | 20k 条/s | 8 快 | split 8192/1024 stride128 spin | 无 |
| ultra-var-max | 变长 64–4096B | **rate=0 最大吞吐** | 8 快 | split 8192/1024 stride128 spin | 无 |
| fixed-4k-premark-4r | 固定 4KiB | 10k 条/s | 4 快 | inline-premark 8192/512 spec | 无 |
| restarts | 变长 128–1024B | 2k 条/s | 4（1 慢 2ms） | inline 2048/2048 futex | writer 120–300s；随机 reader 60–240s |
| slow-vs-fast | 固定 256B | 2k 条/s | 2 快 + 1 慢 5ms | inline 1024/4096 spin | 无 |
| tiny-ring | 固定 16B | 1k 条/s | 2 快 + 1 慢 2ms | split 128/2 spin | 无 |
| large-slot | 固定 1MiB | 100 条/s | 2 快 | inline 2MiB/64 futex | 无 |
| mlock-writerlock | 固定 256B | 1k 条/s | 2 快 | inline 512/512 mlock+writerLock futex | 无 |

外部资源监控（`resource_monitor.py`）每 30 s 采样：RSS/PSS/VmSize/HWM/Swap、CPU%、线程、映射、FD 分类、缺页、上下文切换、IO、smaps 明细；按 runner PPID 关联 worker，每条记录带 lane/role/reader index/pid/seed/slow_us 与实验时间轴。

### 6.2 结果（2026-11-14 09:09 → 2026-11-16 09:50，48.7 h）

原计划 72 h，用户确认时长已满足需要后优雅停止；停止时写入完整汇总。

| 指标 | 结果 |
|---|---:|
| lane 通过 | **10 / 10 PASS，0 violations** |
| 总发送 | **37,666,054,606** 条 |
| reader 累计接收 | **183,174,608,960** 条 |
| corrupt / bad / dup / ooo | **0 / 0 / 0 / 0** |
| writer rejected | **0** |
| writer 实例 / reader 实例 | 838 / 1185 |
| writer 重启 / reader 重启 | 828 / 1147，全部恢复 |

| lane | 实测发送速率（条/秒） | 发送条数 | 丢失（条） | 重启（writer/reader） |
|---|---:|---:|---:|---:|
| fixed-low-2r | 100/s | 17,531,469 | 0 | 0 / 0 |
| fixed-high-2r | 99,999/s | 17,531,379,310 | 0 | 0 / 0 |
| var-high-8r | 20,000/s | 3,506,295,779 | 0 | 0 / 0 |
| ultra-var-max | **78,658/s**（变长 64–4096B） | 13,790,074,275 | 0 | 0 / 0 |
| fixed-4k-premark-4r | 10,000/s | 1,753,148,218 | 0 | 0 / 0 |
| restarts | 1,991/s | 349,001,977 | 263,605,787 | 828 / 1147 |
| slow-vs-fast | 2,000/s | 350,626,183 | 315,953,848 | 0 / 0 |
| tiny-ring | 1,000/s | 175,314,354 | 90,040,196 | 0 / 0 |
| large-slot（1MiB） | 99/s | 17,368,375 | 0 | 0 / 0 |
| mlock-writerlock | 1,000/s | 175,314,666 | 0 | 0 / 0 |

注：表中“100/s”“20k/s”等均表示**条/秒**（每秒发送的消息条数）；“发送条数”与“丢失”的单位均为条。

**丢失归因（逐 reader 实例统计）**：

- restarts：fast reader 丢失 **0**（接收 1,050,931,257 条）；慢 reader（2 ms）丢失 263,605,787 条。
- slow-vs-fast：fast reader 丢失 **0**（接收 701,252,339 条）；慢 reader（5 ms）丢失 315,953,848 条。
- tiny-ring：fast reader 丢失 **17** 条（两实例分别 7、10，接收 350,628,658 条），慢 reader 丢失 90,040,179 条。slotCount=2 时快 reader 也会遇到调度边界，这是 2-slot 环的极端配置，按有损广播解释。

所有 lane 的归因结论：**丢失全部集中在慢 reader（callback 耗时超过生产速率）与重启窗口；无重启的 fast reader 在 restarts 之外均为零丢失**；没有任何一条 corrupt/bad/dup/ooo。

来源：[STABILITY_RESULTS.json](STABILITY_RESULTS.json)、[STABILITY_SUMMARY.json](STABILITY_SUMMARY.json)（原始逐实例汇总）。

### 6.3 资源趋势（48.7 h）

| 指标 | 结果 |
|---|---|
| 采样 | 272,633 条；1,973 个 PID；43 个长期存活 PID 各 ≥1,000 样本 |
| max ΔRSS | **0 KiB** |
| max ΔPSS | **+6 KiB**（噪声级） |
| max ΔFD / Δ线程 / Δ映射 | **0 / 0 / 0** |
| FD 规模 | writer 4、reader 3（长期不变） |
| RSS 规模 | 多数 lane 约 2,000–12,000 KiB；large-slot（2 MiB×64）reader 136,028 KiB、writer 135,676 KiB（约 133 MiB），全程不变 |
| CPU | 高频 spin lane 约 99.9%/进程；低频 futex lane 0.2%；large-slot writer 99.9%、reader 38.1%；mlock-writerlock 约 1.5% |
| 缺页/上下文切换/IO | 持续记录于 `resources.jsonl`；shm 数据面走 mmap，`/proc/pid/io` 主要反映控制面/日志 IO |

来源：[STABILITY_RESOURCE_RESULTS.json](STABILITY_RESOURCE_RESULTS.json)；原始 `/tmp/shm-stability-72h-v2-resources/`。

### 6.4 稳定性结论

在 48.7 h、10 lane 并发、含 828/1147 次随机进程重启的压力下，组件数据面保持完整性（零损坏/重复/乱序），无内存、FD、线程、映射泄漏趋势；丢失仅由慢 reader 与重启窗口解释，且行为符合有损广播语义。原定 72 h 未跑满，24 h 之后的结论延伸依赖线性假设外的实测缺失（见第 7 节）。

### 6.5 跨 NUMA 1 小时稳定性（2026-11-16 补充）

使用 `run_stability.py` 的 4 条跨 NUMA lane：writer 与 reader 进程分别绑定到**互不重叠**的物理核（同节点对照在 NUMA1；跨节点 lane 分别为 NUMA0 writer→NUMA1 readers、NUMA1 writer→NUMA0 reader、NUMA0 writer→NUMA1 readers 的变长场景），runner 固定在 CPU 0、资源监控固定在 CPU 2。运行 **1.0 小时**。

结果：**4/4 lane PASS，0 violations**；总发送 **252,081,920** 条，reader 累计接收 **900,288,080** 条；lost / corrupt / bad / dup / ooo / rejected **全为 0**。

| lane | 配置 | 实测发送速率（条/秒） | 发送条数 | 丢失（条） | reader p99 最大 |
|---|---|---:|---:|---:|---:|
| numa-same-w1-r1-4r | 4 快 reader，20k/s，固定 256B，spin | 19,999/s | 72,023,104 | 0 | 6.78 µs |
| numa-cross-w0-r1-4r | writer NUMA0 → 4 reader NUMA1，20k/s，固定 256B，spin | 19,998/s | 72,022,876 | 0 | 4.48 µs |
| numa-cross-w1-r0-1r | writer NUMA1 → 1 reader NUMA0，10k/s，固定 256B，futex | 9,999/s | 36,011,393 | 0 | 15.2 µs |
| numa-cross-w0-r1-var4r | writer NUMA0 → 4 reader NUMA1，20k/s，变长 64–4096B，split | 19,999/s | 72,024,547 | 0 | 31.7 µs |

注：表中速率均为条/秒；“发送条数”和“丢失”的单位均为条。

资源：**6,001 条采样、17 个长期 PID；ΔRSS / ΔPSS / ΔFD / Δ线程 / Δ映射 全部为 0**。RSS 峰值：spin lane reader 4,524–4,608 KiB、writer 5,688–5,928 KiB；变长 lane reader 10,632 KiB、writer 11,892 KiB。CPU：spin/futex writer 与 spin reader 约 99.9%/进程，futex reader 约 10%（睡眠等待的预期差异）。

说明：这条 lane 主要用于验证跨节点长时间运行的数据完整性、丢失与资源趋势；延迟数字来自独立进程 worker（逐字节 CRC、`spinCount=1`），只作同批次参考，不与 §5.2–5.3 的进程内数字比较。首次启动的 1h 批次因两条 lane 复用同一组 reader 物理核被作废（p50 被放大到约 1.2 ms），该轮数据不进入结论。

---

## 7. 未覆盖场景（明确清单）

以下项目**没有**在本次测试中执行或验证。不得把“未观察到问题”当作这些场景可用的证据。

### 7.1 平台与环境

| 未覆盖项 | 说明 |
|---|---|
| 非 Linux / 非 x86_64 | 组件仅支持 Linux；未测 arm64、其它发行版、其它内核版本（本次仅 4.18.0-372.9.1.el8） |
| NUMA interleave / membind | §5.9 覆盖了 NUMA0/NUMA1 选定物理核的跨节点读写；未测试内存 interleave、`numactl --membind` 策略及其它拓扑 |
| 固定 CPU 频率 | 本机 sysfs 无 governor/cur_freq，未固定频率；绝对数值受频率与共享主机影响 |
| 独占主机 | 未独占；测试期间可能存在其它负载 |
| 容器/命名空间 | 未测容器内 `/dev/shm` 大小限制、IPC namespace 隔离、只读挂载 |

### 7.2 资源耗尽与权限

| 未覆盖项 | 说明 |
|---|---|
| `/dev/shm` 空间耗尽 | 未制造 ENOSPC；只验证了 FD 上限与 mlock 不可用 |
| inode 配额 / 段创建失败 | `shm_open`/`ftruncate` 失败路径未系统覆盖 |
| mmap 失败 | 未在受控条件下制造 MAP_FAILED 并验证错误传播 |
| 权限组合 | 未测不同用户/组、只读段、umask、SELinux/AppArmor 策略 |
| 系统级内存压力 | 未在内存紧张/swap 压力下运行 |

### 7.3 故障与不可信输入

| 未覆盖项 | 说明 |
|---|---|
| 任意损坏头的 fuzz | 只测了若干结构化异常（零长度、短头、odd seq、超长长度、错误 hash、published 越界）；未做随机 fuzz |
| 外部篡改已提交 payload | seqlock 不提供内容完整性保证，属于明确不保证项；未测篡改后的行为 |
| 硬件/内存故障 | 未模拟 ECC、内存错误、位翻转 |
| 锁文件替换 | 仅风险探测（观察到第二 writer 可取得段），未作为支持/不支持契约系统验证 |
| 同段无锁并发写 | 不支持用法，只做风险探测，未做系统性矩阵 |

### 7.4 规模与配置

| 未覆盖项 | 说明 |
|---|---|
| >8 readers | 性能与稳定性最高 8 readers |
| >4 KiB 常规 payload | 1 MiB 只在稳定性 100 条/s lane 验证，未做性能矩阵；未测 16 KiB–1 MiB 的中高频 |
| slotCount>4096 / slotSize 极端值 | 功能覆盖 2/4/64/4096，性能只在 4096；稳定性覆盖 2 与 64–4096，但未做系统性能对照 |
| 非 2 的幂 slotCount | 明确拒绝（校验），未作为可用配置测试 |
| writerLock 性能 | 只在稳定性 lane 做过功能性验证，未进入性能矩阵 |
| mlock 性能影响 | 只在稳定性 lane 验证功能，未量化性能差异 |
| 多 writer 并发（不同进程） | R-writer-conflict 验证互斥与接管；未测“多个成功 writer 同时写”的损坏模式（非支持用法） |

### 7.5 集成与上层用法

| 未覆盖项 | 说明 |
|---|---|
| 单遍回测 App | 传输节点只支持 RealTimeApp；未在 TopDownSinglePass 中实例化（不支持） |
| graph 跨进程性能 | §5.10 覆盖同一进程内的图回环（raw/MultiTransport、1/4 监听端、10k 与饱和）；未测发布者与监听者分属不同进程的图路径性能 |
| Heartbeat 性能 | 未测量心跳频率、TTL 检查、发现规模对数据面的影响 |
| 真实 RPC 链路 | Heartbeat 测试未启动 RPC 服务，只验证消息/注册语义 |
| 重连/接管时延 | R08 验证 1000 次重连的正确性，未量化接管时延分布 |
| 真实业务负载 | 消息为合成报文；未用真实业务大小分布、topic 分布、burst 模式 |
| 跨版本/ABI | 未测老段文件 + 新组件、共享内存段布局版本兼容性 |

### 7.6 验证手段与时长

| 未覆盖项 | 说明 |
|---|---|
| TSan | 未运行；且 TSan 不能证明跨进程共享内存无撕裂 |
| LeakSanitizer | 当前 ptrace 环境报 fatal error，只能 `detect_leaks=0`；**不宣称泄漏检查通过** |
| sanitizer 覆盖范围 | ASan/UBSan 只跑核心 778 项；graph/heartbeat/resource 未在 sanitizer 下运行 |
| 单配置 2 h / 24 h soak | 未执行；多路稳定性是混合配置，不是单配置 soak |
| 原定 72 h | 实际 48.7 h 后停止；未覆盖 48.7–72 h 区间 |
| 崩溃/布局切换混合长跑 | 稳定性含随机进程重启，但未在同一 lane 混合 writer 崩溃与 layout/count/stride 切换长跑 |
| p99.9 | 每 reader 每轮 30 万样本，未报告 p99.9 |
| 性能回归双批复测 | 只有一批 5 轮数据，未做同机两批稳定复现判定 |

---

## 8. 结论

1. **功能**：在已覆盖的 24 组线格式组合、图/Heartbeat 集成和异常/生命周期场景下，组件行为与文档语义一致；三种 layout、两种 read、两种 wait 均有通过记录；精确崩溃窗口与恢复扫描、futex 交错、套圈对账均通过。
2. **已知限制**：DEF-1 是唯一保留的失败项，已明确不修复；资源 smoke 的 1 个 FAIL 和统一入口退出码 1 属预期。H04 风险探测跨轮次三 layout 均出现过，应作为“首次未加锁不能补救”的用法约束。
3. **效率**：在 10k 条/s 与 50%/90% 负载、最多 8 readers、最大 4 KiB 常规 payload 下零丢失，延迟与吞吐满足设定门槛；慢 reader 不增加 writer 成本；futex 空闲 CPU 达标。绝对数值不可跨机比较。
4. **稳定性**：48.7 h 多路并发下无数据损坏/重复/乱序，无资源泄漏趋势，随机重启可恢复。丢失可完全由慢 reader 与重启窗口解释。
5. **覆盖边界**：第 7 节列出的场景（尤其 `/dev/shm` 耗尽、mmap 失败、权限、sanitizer 完整覆盖、>8 readers、graph 性能、72 h 完整时长）**未验证**；上线前如需这些保证，应另行补测。

---

## 9. 证据索引

| 文件/目录 | 内容 |
|---|---|
| [UNIFIED_RESULTS.json](UNIFIED_RESULTS.json) | 统一功能验收：命令、环境、二进制哈希、各套件计数、DEF-1 定位 |
| [PERF_BASELINE.json](PERF_BASELINE.json) | staged 性能基线（26 配置）与慢 reader/futex 阈值 |
| [LOAD_RESULTS.json](LOAD_RESULTS.json) | 饱和吞吐与 50%/90% 负载 |
| [PERF_FULL_RESULTS.json](PERF_FULL_RESULTS.json) | 全量 240 配置 × 5 轮性能矩阵 |
| [STABILITY_RESULTS.json](STABILITY_RESULTS.json) | 48.7 h 稳定性按 lane 汇总 |
| [STABILITY_SUMMARY.json](STABILITY_SUMMARY.json) | 稳定性原始逐实例汇总 |
| [STABILITY_RESOURCE_RESULTS.json](STABILITY_RESOURCE_RESULTS.json) | 长期进程资源增长与 CPU/IO/缺页摘要 |
| [PERF_NUMA_RESULTS.json](PERF_NUMA_RESULTS.json) | 跨 NUMA 性能：16 配置 × 5 轮，同节点对照与跨节点对比 |
| [STABILITY_NUMA_RESULTS.json](STABILITY_NUMA_RESULTS.json) | 跨 NUMA 1h 稳定性：4 lane 数据完整性与资源摘要 |
| [GRAPH_PERF_RESULTS.json](GRAPH_PERF_RESULTS.json) | 图/MultiTransport 性能：12 配置 × 5 轮 |
| [RESOURCE_RESULTS.json](RESOURCE_RESULTS.json) | 10k churn（含 DEF-1）与单轮性能证据 |
| [DEF1_FIX_EXPERIMENT.json](DEF1_FIX_EXPERIMENT.json) | DEF-1 修复实验（已回滚，不进入交付） |
| [heartbeat-release-results.json](heartbeat-release-results.json) / [heartbeat-risk-results.json](heartbeat-risk-results.json) | Heartbeat 与 H04 风险证据 |
| [PLAN.md](PLAN.md) / [README.md](README.md) | 测试方案与实施记录 |
| `/tmp/shm-func-*`、`/tmp/shm-perf-*`、`/tmp/shm-stability-72h-v2*` | 原始逐例日志、参数与 stdout/stderr（本机临时目录，未入库） |

---

## 10. 复现入口

```sh
cd /data/hxli/codes/tm_kit/build-common && source ./build-env-setting.sh
cd /data/hxli/codes/tm_kit/worktrees/shm-seqlock-system-test

# 功能 + 图 + Heartbeat + 资源 + 基线（release）
meson setup builddir_suite_release test/shm_seqlock_system_test/suite --buildtype=release
ninja -j4 -C builddir_suite_release
python3 test/shm_seqlock_system_test/run_suite.py builddir_suite_release \
  --matrix --baseline --out=/tmp/shm-suite-unique

# 全量性能矩阵（约 12 小时）
python3 test/shm_seqlock_resource_system_test/run_tests.py \
  builddir_suite_release/shm_seqlock_resource_system_test \
  --profile=full --repeat=5 --out=/tmp/shm-perf-unique

# 跨 NUMA 性能基准（16 配置 × 5 轮，约 47 分钟）
python3 test/shm_seqlock_stability_test/cross_numa_bench.py \
  builddir_suite_release/shm_seqlock_stability_worker --out=/tmp/shm-numa-perf

# 跨 NUMA 1 小时稳定性（4 lane，互不重叠的物理核；runner/monitor 固定在控制核）
taskset -c 0 python3 test/shm_seqlock_stability_test/run_stability.py \
  builddir_suite_release/shm_seqlock_stability_worker \
  --out=/tmp/shm-stability-numa-unique --hours 1 \
  --lanes numa-same-w1-r1-4r,numa-cross-w0-r1-4r,numa-cross-w1-r0-1r,numa-cross-w0-r1-var4r
taskset -c 2 python3 test/shm_seqlock_stability_test/resource_monitor.py \
  --out=/tmp/shm-stability-numa-unique --monitor-out=/tmp/shm-stability-numa-unique-resources \
  --interval 10 --hours 1

# 图/MultiTransport 性能基准（12 配置 × 5 轮，约 35 分钟）
ninja -j4 -C builddir_suite_release shm_seqlock_graph_bench
python3 test/shm_seqlock_graph_bench/run_bench.py \
  builddir_suite_release/shm_seqlock_graph_bench --out=/tmp/shm-graph-bench

# 多路稳定性（示例：48 小时）+ 独立资源监控
python3 test/shm_seqlock_stability_test/run_stability.py \
  builddir_suite_release/shm_seqlock_stability_worker \
  --out=/tmp/shm-stability-unique --hours 48
python3 test/shm_seqlock_stability_test/resource_monitor.py \
  --out=/tmp/shm-stability-unique --monitor-out=/tmp/shm-stability-unique-resources \
  --interval 30 --hours 48
```

说明：统一套件退出码 1 由 DEF-1 引起，属预期；其余套件应全部为 0。
