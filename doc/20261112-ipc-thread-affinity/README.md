# IPC Transport 内部线程通过 ConnectionLocator 绑核

**任务开始时间：** 2026-11-12  
**任务结束时间：** 2026-11-13  
**状态：** 完成  
**分支：** `feature/ipc-thread-affinity`（worktree `worktrees/tm_transport-ipc-thread-affinity`，起点 `e22df3c`，实现 commit `ccc0a58`）

## 背景

`shm_seqlock`、`shm_cursor`、`shared_memory_broadcast`、`aeron_ipc`、`zeromq(ipc)`、`nng(ipc)` 等 IPC transport
在组件内部各自创建接收/轮询线程，`multicast` / `singlecast` 还有组件级共享发送线程。这些线程目前只能由外部
`taskset` / `numactl` 整体约束进程，无法单独分配到核心，低延迟部署时不能把 hot thread 与框架/杂务线程分开。

tm_infra 已有一套完整的绑核设施（`CpuBindingSpec`、`IThreadCpuAllocator`、`ThreadCpuLease`、
`ThreadCpuAllocatorComponent`），tm_basic 有 hwloc 实现（`HwlocThreadCpuAllocator`，含 SMT/NUMA 感知与显式独占）。
本任务让 transport 内部线程复用这套设施。

## 目标

1. 每个有内部线程的 IPC transport 组件新增注入接口 `xxx_setThreadCpuLease(ThreadCpuLeaseProvider)`，
   provider 签名为 `(who, spec) -> infra::ThreadCpuLease`，与 `ThreadCpuLease::bindCurrentThreadBy` 一致。
   provider 只能设置一次，已设置时抛 `std::logic_error`。
2. ConnectionLocator 新增属性 `cpu`（接收/轮询线程）、`senderCpu`（multicast/singlecast 共享发送线程）、
   `driverCpu`（aeron 内嵌 driver）。属性值使用 `CpuBindingSpec` 语法。
3. 各 `XxxImporterExporter` 模板与 `MultiTransportBroadcastListener` 节点在启动时以“没装才装”的方式安装
   默认 provider（`threadCpuLeaseProviderFor<Env>()`）。
4. tm_infra / tm_basic 不改动；不设置绑核属性时行为与现在完全一致。

## 方案

完整方案见 [PLAN.md](PLAN.md)。要点：

- 绑核发生在工作线程自身：线程函数第一行调用 provider 取得 `ThreadCpuLease`，租约随线程函数退出而析构归还。
- provider 的异常通过启动握手（promise/future）同步抛回 `addSubscriptionClient` / `getPublisher` 的调用者。
- 共享线程首个 locator 的 `cpu` 生效，后续不同值只打警告。
- aeron 内嵌 driver 的 agent 线程由 Aeron 通过 C 回调启动，不能抛异常：失败打 stderr 并计入
  `AeronIpcPublisherStats::driverBindFailed`。

## 代码改动（按文件列出）

### 新增文件

| 文件 | 内容 |
|---|---|
| `tm_kit/transport/ThreadCpuLeaseProvider.hpp` | `ThreadCpuLeaseProvider` 别名、`threadCpuLeaseProviderFor<Env>()`、`installThreadCpuLeaseProviders<Env>()` 前向声明；组件头只需要这些。 |
| `tm_kit/transport/ThreadCpuLeaseProviderInstall.hpp` | `installThreadCpuLeaseProviders<Env>()` 定义：按 `is_convertible_v<Env *, XxxComponent *>` 逐组件调用 `xxx_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>())`。单独成文件避免组件头与安装器互相包含。 |
| `src/ThreadCpuLeaseSupport.hpp` | 内部：`BindRequest`、`makeThreadCpuBindRequest[FromSpec]`（读属性、`CpuBindingSpec::parse` 校验、缺 provider 报错）、`sameCpuBinding`（共享线程比较）、`BindHandshake`、`bindWorkerThread`（线程第一行绑核并把结果/异常交给调用者）。 |
| `test/ipc_thread_affinity_test/{IpcThreadAffinityTest.cpp,meson.build,run_tests.sh}` | 287 项检查的功能/稳定性测试，见下。 |

### 组件（`tm_kit/transport/.../XxxComponent.hpp` + `src/Xxx/XxxComponent.cpp`）

统一改动：Impl 持有 `ThreadCpuLeaseProvider` + `已设置` 标志（`providerMutex_` 保护），公共
`xxx_setThreadCpuLease`（重复设置抛 `std::logic_error`）+ 私有 `xxx_setThreadCpuLeaseIfUnset`
（friend `installThreadCpuLeaseProviders`）。订阅/发送线程在启动处创建握手，线程第一行
`bindWorkerThread`；失败时构造/注册路径清理刚创建的对象并原样抛出 provider 的异常。

| 组件 | 改动 |
|---|---|
| `shm_seqlock` | 订阅构造接收 `BindRequest`（在 `addSubscriptionClient` 调用线程里构建）；懒启动读线程时握手；绑定失败则 `addSubscriptionClient` 删除刚插入的订阅再抛出。共享读线程按 identifier 复用，不同 `cpu` 只警告一次。 |
| `shm_cursor` | 订阅构造接收 `BindRequest`，线程 lambda 第一行绑核 + 握手；绑核失败 join 线程后抛出，订阅不入缓存。 |
| `shared_memory_broadcast` | 构造接收 `BindRequest`；**先绑核（握手）再向共享内存登记 client 项**，失败时不留半登记的 client；之后缓存 native handle 再 detach。 |
| `zeromq` / `nng` | `getOrStartSubscription` 在裁剪 locator 前读 `cpu`，构造接收 `BindRequest`；共享接收线程不同 `cpu` 警告一次。 |
| `multicast` | 接收线程构造接收 `BindRequest`（先 join-on-failure 再抛出）；共享发送线程在首次 `getOrStartSender` 时绑 `senderCpu` 并握手，后续不同值警告一次。 |
| `singlecast` | 接收线程改为在 lambda 内绑核（握手在 detach 之前）；共享发送线程同 multicast。 |
| `aeron_ipc` | 订阅 poll 线程接收 `BindRequest`；删除私有 `parseCpuList`/`bindCurrentThread`，`driverCpu` 改走 provider；`AeronIpcPublisher` 在 `onDriverAgentStart` 里按 roleName 逐个 agent 取租约（存于 mutex 保护的 `vector<ThreadCpuLease>`，`aeron_driver_close` 后清空），失败打 stderr 并计入新增的 `AeronIpcPublisherStats::driverBindFailed`。`TM_AERON_IPC_DRIVER_CPU` 回退保留。 |

### 安装点

- 各 `XxxImporterExporter.hpp`：4 个 `start(Env *)` 和 2 个 `fetch[Typed]FirstUpdateAndDisconnect` 入口都先
  `installThreadCpuLeaseProviders<Env>(env)`。
- `MultiTransportBroadcastListener.hpp` 的 `start(Env *)`：该节点直接调组件 API，自行安装。
- `HeartbeatAndAlertComponent.hpp` 的 `initializeHeartbeatAndAlertComponent`：该函数直接调
  `xxx_getPublisher`，也补装一次（用户确认的 PLAN 外补充）。
- `MultiTransportBroadcastListenerManagingUtils` / `MultiTransportBroadcastPublisherManagingUtils` 零改动。

### 顺带修复（测试暴露的既有问题）

- `SinglecastComponent`：`th.detach()` 之后调用 `th.native_handle()`（libstdc++ 在 detach 时清空句柄），
  `singlecast_threadHandles()` 一直返回 0。改为 detach 前缓存。
- `SharedMemoryBroadcastComponent`：同样缓存 handle；另外析构原先对已 detach 的线程调用 `join()`
  （必然抛异常被吞掉），随后立刻释放接收线程正在使用的共享内存，取消订阅时可稳定复现 SIGSEGV。
  改为不 detach、析构时 join（从自身回调析构时 detach），并把订阅销毁移到组件锁之外
  （与 multicast 一致），避免回调重入组件时死锁。

### 实现记录

- **2026-11-12 方案细化（实现前）**
  - 新增安装头拆成两个：`ThreadCpuLeaseProvider.hpp`（别名 + `threadCpuLeaseProviderFor` +
    `installThreadCpuLeaseProviders` 前向声明）和 `ThreadCpuLeaseProviderInstall.hpp`（安装器定义）。
    原因：组件头需要 provider 别名，而安装器需要全部组件头，放在同一个头会形成包含环。
  - `xxx_setThreadCpuLeaseIfUnset` 保持 private，组件类内 `friend` 安装器模板（`transport` 命名空间里
    的前向声明在基座头中给出）。
  - `initializeHeartbeatAndAlertComponent` 入口补一次 `installThreadCpuLeaseProviders<Env>(env)`
    （经用户确认，PLAN 原写“不改”）。原因：该函数直接调 `xxx_getPublisher`，不经过任何
    ImporterExporter/Listener 的 `start()`，否则 heartbeat locator 上的 `cpu` 属性会因没有 provider 抛异常。
  - shm_cursor 的 `cpu` 属性直接从 locator 读、在 `addSubscriptionClient` 里构建绑定请求，未塞进
    `ShmCursorOptions`：同一 segment 的后续 locator 也要参与“共享线程只能绑一次”的比较，而
    `ShmCursorOptions::fromLocator` 只在首次创建时调用。
  - 启动握手只在确实需要绑核时创建（promise/future），`none`/空属性零额外开销。

## 测试与验证（命令、构建类型、结果）

新增测试：`test/ipc_thread_affinity_test/`。

```bash
# 构建
cd build-common && source ./build-env-setting.sh
cd ../worktrees/tm_transport-ipc-thread-affinity
ninja -C builddir_debug -j2 test/ipc_thread_affinity_test/ipc_thread_affinity_test
ninja -C builddir_release -j2 test/ipc_thread_affinity_test/ipc_thread_affinity_test

# 运行（需要进程 affinity 里至少 4 个 CPU；IPC_AFFINITY_CASE=<transport> 可只跑一组）
./test/ipc_thread_affinity_test/run_tests.sh ./builddir_release/test/ipc_thread_affinity_test/ipc_thread_affinity_test
```

**结果（2026-11-12，release 与 debug 各一次）：287 checks, 0 failed。**

覆盖内容（每个 transport：shm_seqlock / shm_cursor / shared_memory_broadcast / zeromq / nng /
multicast / singlecast / aeron_ipc；aeron driver 与 hwloc 按构建条件追加）：

1. `cpu=auto`：Fake allocator 的 `acquire` 恰好一次，工作线程 `pthread_getaffinity_np` 等于分配到的 CPU；
   `who` 里带 transport 名；取消订阅后租约释放（aeron 订阅线程按现有设计保留到组件销毁，单独断言）。
2. `cpu=cpus:<n>`：走 `reserve` 而不是 `acquire`，线程绑到该 CPU，取消订阅后释放。
3. `cpu=none` / 不写 cpu：不调用 provider，线程保持继承的 affinity。
4. 语法错误 `cpu=abc` / `cpu=-1`：`std::invalid_argument`，消息含属性名、值和 locator。
5. 没有 provider：非 none 属性抛 `std::invalid_argument` 且不留线程；不带属性正常。
6. provider 抛自定义异常：`addSubscriptionClient` 原样抛出同一类型，不留缓存线程，之后不带属性的订阅正常。
7. 重复设置 provider：第二次抛 `std::logic_error`，原 provider 继续生效。
8. Env 无 `ThreadCpuAllocatorComponent`：默认 provider 对 `cpu=<n>` 抛 `ThreadCpuBindError`，`cpu=none` 正常。
9. 共享线程：同一 key 两个 locator 不同 cpu，第一个生效、第二个只打一次 `already uses` 警告、只 reserve 一次；
   `senderCpu` 对 multicast/singlecast 同理。
10. 模板路径：不手工设置 provider，直接 `ZeroMQImporterExporter<Env>::createImporter` + `AppRunner::finalize`，
    `start(env)` 自动安装 provider，`cpu=auto` 生效。
11. 稳定性：每个 transport 10 次 subscribe/unsubscribe 循环，结束时无残留租约。
12. hwloc（`TEST_HAS_HWLOC`）：`installThreadCpuLeaseProviders<HwlocEnv>` 后 `cpu=auto` 绑到单核；
    再用 `cpus:<该核>` 绑第二个 transport 抛 `CpuAutoBindError`（hwloc 独占语义）。
13. aeron（`TM_KIT_TRANSPORT_AERON_IPC`）：`driverCpu=auto` + 订阅 `cpu=auto` 各自 acquire，
    `driverBindFailed=0`；销毁环境后租约全部释放；非法 `driverCpu` 与缺 provider 都在调用线程
    `std::invalid_argument`。

对测试设计的两点缩减（相对 PLAN）：
- hwloc 用例不再启动 `CpuAutoBind()` 图节点比较“不同物理核”，改为验证 transport 线程与 hwloc
  分配器对同一核的独占冲突（同样覆盖分配器集成与互斥）。
- 线程绑定断言用 `pthread_getaffinity_np(threadHandles())`，不扫描 `/proc/self/task/*/status`；
  在修复 detach 后句柄失效的问题之后两者等价（见“顺带修复”）。

回归结果见下一小节。

### 回归结果（release，2026-11-12）

| 测试 | 结果 |
|---|---|
| `ipc_thread_affinity_test`（新增） | 287 checks / 0 failed（release 与 debug 各一次） |
| `shm_seqlock_test/run_tests.sh` | pass=24 fail=0 |
| `shm_cursor_test/run_tests.sh` | pass=31 fail=0 |
| `aeron_ipc_test/run_tests.sh` | pass=7 fail=1：仅 test3（慢读者场景要求 fast reader 全收 50 万条零 gap）失败；用基点 `e22df3c` 的未修改 release 二进制单跑同一用例同样失败（fast received 78367 vs 我们 78381），属既有/环境问题（共享机器 + 该用例的零丢失断言），与本次改动无关 |
| `ipc_concurrency_test/run_tests.sh` | pass=19 fail=0 risk=15 safe=5；RISK 是脚本本身标注的“不支持的并发写法”观察项（含 aeron 未加锁多写的 SIGSEGV），不是本轮引入 |
| `multitransport_lifecycle_test/run_tests.py` | 3 个 transport 全部 PASS |
| `multicast_test` | pass=6 fail=0 |
| `zeromq_test` | pass=11 fail=0 |
| `transport_payload_test` | failures=0 |
| `threaded_handler_transport_test` | ALL PASSED |
| `tm_examples` debug（清理重建） | 291/291 目标构建/链接成功，下游头文件兼容 |

debug 构建下 `shm_seqlock_test`（零 gap 断言）和 `shm_cursor_test`（限速时间断言）有既有的时序失败，
用基点 `e22df3c` 的 debug 二进制跑同样的用例得到同样的失败，确认与本次改动无关；这两个用例只适合
release/空闲机器上做验收。

PLAN 中的性能参考项（`ipc_latency_profiler` 内部 `cpu=` 绑核与外部 `taskset` 对比）未执行：
该项本来就不作为验收条件，且当前机器为共享环境，延迟数字不可比。

## 兼容性与注意事项

- 未设置 `cpu` / `senderCpu` / `driverCpu` 时，线程创建路径、异常行为和性能与改动前一致；
  握手对象只在确实需要绑核时创建。
- provider 一旦设置不能更换或清除：自定义 provider 必须在图启动前设置。框架在
  `XxxImporterExporter::start` / `fetch...` / `MultiTransportBroadcastListener::start` /
  `initializeHeartbeatAndAlertComponent` 里以“没装才装”的方式补默认值。
- provider 缺失 + 非 `none` 属性 → `std::invalid_argument`；属性语法错误 → `std::invalid_argument`
  （信息含 transport、属性名、值、locator）；Env 无 allocator 的默认 provider → `ThreadCpuBindError`
  （infra 既有行为）。
- 共享线程（按 host/port/identifier 或 identifier 复用）只认第一个 locator 的 `cpu` / `senderCpu`，
  后续不同值只在 stderr 警告一次；aeron_ipc 按完整 locator 分线程，不受此限。
- aeron 内嵌 driver 的 agent 线程由 Aeron 的 C 回调启动，不能抛异常：绑定失败打 stderr 并计入
  `AeronIpcPublisherStats::driverBindFailed`，driver 继续运行。dedicated 模式每个 agent 各取一次租约，
  `driverCpu=auto` 会占 3 个核；显式列表对每个 agent 是同一份列表，严格分配器（hwloc）会拒绝第二个
  reserve（计入 driverBindFailed），dedicated 模式建议用 `auto`。
- 非 Linux：默认 provider 绑定抛 `ThreadCpuBindError`，经启动握手同步到调用者；`none`/空值不受影响。
- `ConnectionLocator` 属性值不能含逗号，CPU 列表用区间写法（`4-5`、`2:5`）。
- 行为变化（不破坏 API）：
  - `shared_memory_broadcast` 取消订阅现在会等接收线程退出后再释放共享内存（futex 等待模式最长约 1s）；
    之前 detach 后立即释放，取消订阅时可复现 use-after-free/SIGSEGV。订阅销毁移到组件锁之外。
  - `singlecast` / `shared_memory_broadcast` 的 `threadHandles()` 现在返回有效 native handle
    （之前 `detach()` 清空了 std::thread 的句柄，返回 0）。

## 相关文档

- [方案](PLAN.md)
- 前序：[ZeroMQ 通过 ConnectionLocator 配置 HWM](../20261105-zeromq-hwm-locator/README.md)（同为 locator 属性扩展）
- 前序：[IPC Transport 并发测试](../20261103-ipc-concurrency-test/README.md)（各 transport 线程模型）
- 设施来源：[节点线程自动绑核 CpuAutoBind](../../../tm_infra/doc/20261111-thread-cpu-autobind/README.md)（`ThreadCpuLease`）
