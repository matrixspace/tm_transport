# 方案：IPC transports 内部线程通过 ConnectionLocator 绑核

## 目标

让 tm_transport 中带内部线程的 IPC transport 组件能把线程绑定到指定 CPU，绑核参数通过
ConnectionLocator 属性给出，绑核能力复用 tm_infra 的 `ThreadCpuLease` / `IThreadCpuAllocator`
（tm_basic 的 hwloc 实现负责自动选核与独占检查），不另造一套 `pthread_setaffinity_np`。

**tm_infra / tm_basic 不改动。** 不设置绑核属性时行为与现在完全一致。

## 设计要点

- 每个纳入范围的 transport 组件新增一个注入接口
  `xxx_setThreadCpuLease(ThreadCpuLeaseProvider)`，provider 类型为
  `std::function<infra::ThreadCpuLease(std::string const &who, std::string const &spec)>`，
  与 `ThreadCpuLease::bindCurrentThreadBy` 的参数一致：
  `who` 是线程描述（如 `shm_seqlock reader feed`、`multicast sender`、`aeron_ipc driver conductor`），
  `spec` 是 locator 属性值原文（`CpuBindingSpec` 语法）。
- **provider 只能设置一次**，不支持更换，也没有清除接口。已设置时 `xxx_setThreadCpuLease` 抛
  `std::logic_error`。框架自身的自动安装走组件私有入口 `xxx_setThreadCpuLeaseIfUnset`，语义为
  “没装才装”，重复调用无副作用。因此**用户自定义 provider 必须在图启动前设置**。
- 绑核发生在工作线程自身：线程函数第一行调用 provider 取得租约，租约作为线程函数局部变量持有，
  线程退出即析构归还。`ThreadCpuLease` 本来就只绑调用线程，这个位置是唯一正确的位置，
  也避开了 `singlecast` / `shared_memory_broadcast` 线程 detach 后句柄失效的问题。
- provider 在工作线程里抛出的异常通过启动握手（promise/future）**同步**交回
  `addSubscriptionClient` / `getPublisher` 的调用者：调用线程阻塞到绑核结果返回，失败时清理刚创建的
  订阅对象并把异常原样抛出（`ThreadCpuBindError` / `CpuAutoBindError` / `std::invalid_argument` 不包装），
  不留半启动状态。aeron 内嵌 driver 的 agent 线程例外，见下。
- 不带绑核属性或属性为 `none`：不调 provider，不做握手，零额外开销。

## 属性

| 属性 | 适用组件 | 作用对象 |
|---|---|---|
| `cpu` | 全部纳入范围的组件 | 每 locator 一个的接收/轮询线程 |
| `senderCpu` | multicast、singlecast | 组件级共享发送线程 |
| `driverCpu` | aeron_ipc | 内嵌 media driver 的 agent 线程 |

取值使用 `CpuBindingSpec` 语法：`none` / 空（默认，不绑）、`auto`、`numa:<n>`、`cpus:<list>`、`<list>`
（`4`、`2-5`、`2:5`）。ConnectionLocator 属性值不能含逗号，所以 CPU 列表用区间写法。

- 绑定效果由 provider 决定。Env 带 `basic::HwlocThreadCpuAllocatorComponent` 时，`auto` / `numa:<n>`
  由分配器选核，显式列表走 `reserve` 并与图节点的自动绑核互相避让（撞同一物理核抛 `CpuAutoBindError`，
  这是 hwloc 分配器既有的独占语义）。Env 无分配器时，`bindCurrentThreadBy` 对非 `none` 的 spec 抛
  `ThreadCpuBindError`（infra 既有行为）。
- 组件没有 provider 却写了非 `none` 的绑核属性：抛 `std::invalid_argument`，信息提示调用
  `xxx_setThreadCpuLease` 或 `installThreadCpuLeaseProviders`。不退化为直接 pin。
- 属性语法错误：抛 `std::invalid_argument`，信息含 transport 名、属性名、值、locator，与 zeromq `hwm`
  的处理一致。语法校验在创建线程之前的调用线程里完成。
- 共享线程：按 host/port/identifier 共享的订阅线程，首个创建者的 `cpu` 生效，后续 locator 的不同值只打
  一条 stderr 警告。`senderCpu` 同理，首个 `getOrStartSender` 生效。aeron_ipc 按完整 locator 分线程，
  不受此限。
- `driverCpu` 语法与其它属性统一（旧写法 `2`、`2-3`、`2:3` 仍然合法），`TM_AERON_IPC_DRIVER_CPU`
  环境变量回退保留。每个 agent 线程（shared 模式 1 个，dedicated 模式 3 个）在 `onDriverAgentStart`
  里各取一次租约，`who` 带 Aeron 的 roleName；租约存于 publisher 对象的
  `std::vector<ThreadCpuLease>`（mutex 保护），`closeEmbeddedDriver()` 在 `aeron_driver_close` 之后清空。
  C 回调不能抛异常：失败时打 stderr 并计入 `AeronIpcPublisherStats::driverBindFailed`，driver 继续运行。
  `auto` 下 dedicated 模式会占 3 个核，头文件注释写明。
- 非 Linux：`bindCurrentThreadBy` 抛 `ThreadCpuBindError`，经启动握手同步到调用者。

示例：

```
shm_seqlock://::::feed[wait=spin,cpu=4]
shm_cursor://::::feed[slotSize=512,cpu=cpus:4-5]
shm_seqlock://::::feed[cpu=auto]                 # Env 需带 basic::HwlocThreadCpuAllocatorComponent
aeron_ipc://::::feed[driverCpu=2,cpu=numa:0]
zeromq://ipc:0:::/tmp/feed[rcvhwm=0,cpu=6]
multicast://239.0.0.1:12345[interface=lo,cpu=6,senderCpu=8]
```

## 覆盖范围

| transport | 线程 | 属性 | 新接口 |
|---|---|---|---|
| shm_seqlock | 每 identifier 一个读线程（懒启动） | `cpu` | `shm_seqlock_setThreadCpuLease` |
| shm_cursor | 每 identifier 一个读线程 | `cpu` | `shm_cursor_setThreadCpuLease` |
| shared_memory_broadcast | 每 identifier 一个接收线程 | `cpu` | `shared_memory_broadcast_setThreadCpuLease` |
| aeron_ipc | 订阅 poll 线程（每完整 locator 一个）；内嵌 driver agent 线程 | `cpu`；`driverCpu` | `aeron_ipc_setThreadCpuLease` |
| zeromq | 每 host+port+identifier 一个接收线程 | `cpu` | `zeroMQ_setThreadCpuLease` |
| nng | 每 host+port+identifier 一个接收线程 | `cpu` | `nng_setThreadCpuLease` |
| multicast | 每 host:port 接收线程；组件级共享发送线程 | `cpu` / `senderCpu` | `multicast_setThreadCpuLease` |
| singlecast | 每 host:port 接收线程；组件级共享发送线程 | `cpu` / `senderCpu` | `singlecast_setThreadCpuLease` |

不在范围：redis / rabbitmq / nats / websocket / grpc / socket_rpc（纯网络），
lock_free_in_memory_shared_chain（无内部线程）。

## 代码改动

### 新增文件（tm_transport）

- `tm_kit/transport/ThreadCpuLeaseProvider.hpp`（安装，登记到 `tm_kit/transport/meson.build`）：
  - `using ThreadCpuLeaseProvider = std::function<infra::ThreadCpuLease(std::string const &, std::string const &)>;`
  - `template <class Env> ThreadCpuLeaseProvider threadCpuLeaseProviderFor()`：转调
    `ThreadCpuLease::bindCurrentThreadBy<Env>`。
  - `template <class Env> void installThreadCpuLeaseProviders(Env *env)`：对 `Env` 里存在的每个受支持组件
    调用其 `setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>())`，结构与
    `networkTransportComponents_threadHandles` 相同的 `if constexpr (is_convertible_v<Env *, Xxx *>)` 形式。
- `src/ThreadCpuLeaseSupport.hpp`（内部，不安装）：
  - `makeThreadCpuBindRequest(locator, property, who, provider, transportName)`：读属性、`CpuBindingSpec::parse`
    做语法校验、检查 provider 是否缺省；返回“是否需要绑核 + who + spec + provider”的小结构。
  - 启动握手类型：提供 `promise<void>` / `future<void>` 配对，工作线程绑核后 `set_value` 或
    `set_exception`，调用线程 `get()` 并把异常重新抛出。

### 各组件（`XxxComponent.hpp` / `XxxComponent.cpp`）

统一模式：组件 Impl 持有 provider 与一个“已设置”标志，`xxx_setThreadCpuLease` 设置一次并 PImpl 转发
（已设置抛 `std::logic_error`），`xxx_setThreadCpuLeaseIfUnset` 只在为空时写入；类注释补属性说明。

- `src/shm_seqlock/ShmSeqlockComponent.cpp`：`OneShmSeqlockSubscription` 构造时生成 bind request；
  `addSubscription` 懒启动读线程处加握手（`run()` 第一行绑核，在 `openAndJoin()` 之前）；失败则删除刚建的
  订阅并抛出；后续 locator 的 `cpu` 不同只警告。
- `src/shm_cursor/ShmCursorComponent.cpp`：`ShmCursorOptions` 增加 `cpuSpec`（`fromLocator` 读入）；
  `ShmCursorSubscription` 构造接收 request，线程 lambda 第一行绑核 + 握手。
- `src/shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp`：`getOrStartSubscription` 生成 request 传入
  构造；`run()` 第一行绑核；握手在 `detach()` 之前完成。
- `src/aeron_ipc/AeronIpcComponent.cpp`：删除私有 `parseCpuList` / `bindCurrentThread`；`driverCpu` 改走
  provider（见上）；`AeronIpcSubscriber` 构造生成 `cpu` request，`run()` 第一行绑核 + 握手。
- `src/zeromq/ZeroMQComponent.cpp`、`src/nng/NNGComponent.cpp`：`getOrStartSubscription` 在裁剪 locator 之前
  读属性（与 `rcvhwm` 同处），作为构造参数传入；`run()` 第一行绑核 + 握手。
- `src/multicast/MulticastComponent.cpp`、`src/singlecast/SinglecastComponent.cpp`：接收 io_context 线程绑
  `cpu`；共享发送线程绑 `senderCpu`（首个 `getOrStartSender` 生效，后续只警告）；singlecast 接收线程改为在
  lambda 内自绑。

### provider 安装点

| 文件 / 位置 | 说明 |
|---|---|
| 各 `XxxImporterExporter.hpp` 的 `start(env)` 与静态订阅辅助函数 | 在调用 `xxx_addSubscriptionClient` / `xxx_getPublisher` 之前 `installThreadCpuLeaseProviders<Env>(env)` |
| `MultiTransportBroadcastListener.hpp` 节点的 `start(Env *)` | 节点绕过 ImporterExporter 直接调组件 API（`actuallyHandle` 的 AddSubscription / RemoveAllSubscriptions 分支），必须在节点自身安装 |
| `HeartbeatAndAlertComponent.hpp` | 不改：`xxx_getPublisher` 走组件 API，provider 由 Env 统一安装即可 |

**MultiTransportBroadcastListenerManagingUtils / MultiTransportBroadcastPublisherManagingUtils 零改动。**
两个文件不直接调用 transport 组件 API，所有路径都收敛到下层入口：

| 位置 | 下层入口 | provider 来源 |
|---|---|---|
| `setupOneBroadcastListener_internal`、`oneByteDataBroadcastListener` | `XxxImporterExporter<Env>::create[Typed]Importer` | ImporterExporter `start(env)` |
| `fetch[Typed]FirstUpdateAndDisconnect` | `XxxImporterExporter<Env>::fetchFirstUpdateAndDisconnect(env, ...)` | 该静态辅助函数内安装 |
| `setupBroadcastListener[WithTopic]ThroughHeartbeat` | 构造 `MultiTransportBroadcastListener<Env,T>` 节点 | 节点 `start` 内安装 |
| `one[ByteData]BroadcastPublisher`（含 `WithProtocol`） | `XxxImporterExporter<Env>::create[Typed]Exporter` | Exporter `start(env)` |

multicast / singlecast 的发送线程由 `getPublisher` 触发创建，`start(env)` 先装 provider 再调 `getPublisher`，
`senderCpu` 在首次创建时被读取，顺序正确。该前提需在这两个文件的注释中写明：后续若在此新增直接调用组件 API
的路径，必须自行安装 provider。

`MultiTransportFacilityWrapper` / `MultiTransportRemoteFacility` / `SyntheticMultiTransportFacility` 等上层工具
最终都经由上述节点或 `XxxImporterExporter`，没有其他绕过路径（已核对 `_addSubscriptionClient(` /
`_getPublisher(` 的全部调用点）。

## 兼容性

- 不设置绑核属性时，所有组件的线程创建路径、异常行为、性能与现在一致。
- `xxx_setThreadCpuLease` 是唯一新增的组件公共 API，只增不改。
- `driverCpu` 的取值语法向后兼容（旧的单个 CPU、`a-b`、`a:b` 写法继续有效），
  `TM_AERON_IPC_DRIVER_CPU` 环境变量回退保留。
- 不改 `ConnectionLocator` 解析规则，不改 `*_threadHandles()` 接口。
- 一旦 provider 被设置无法撤销，这是有意的限制：避免运行期改变绑核语义带来不可预期的在线行为变化。

## 测试：`test/ipc_thread_affinity_test/`

`IpcThreadAffinityTest.cpp` + `meson.build`（`shm_cursor_test` 的构建模式，带 `-lrt`、`protobuf`；
`dependency('hwloc', required: false)` 可用时加 `-DTEST_HAS_HWLOC`）+ `run_tests.sh`，在
`test/meson.build` 的 linux 块内注册。Fake 分配器从
`tm_infra/test/threaded_handler_test/ThreadedNodeOptionsTest.cpp` 复制一份。

验证方式：订阅建立后扫描 `/proc/self/task/*/status` 的 `Cpus_allowed_list`，断言预期数量的线程绑到指定列表、
其余线程掩码不变；再收发消息确认功能正常。

用例：

1. Fake 分配器 provider：每个 transport（含 aeron，条件编译）`cpu=auto` 调 acquire 并绑到返回的 CPU；
   `cpus:4` / `4-5` 调 reserve 并绑定；`cpus:` 前缀、`none`、空值；取消订阅、线程结束后 release 被调用。
2. 同步失败：provider 抛异常时 `addSubscriptionClient` / `getPublisher` 原样抛出同类型异常，之后无残留线程、
   订阅缓存为空，再次订阅（不带 `cpu`）正常；语法错误（`abc`、`-1`）抛 `std::invalid_argument`。
3. 无 provider：`cpu=4` 抛 `std::invalid_argument`；不带 `cpu` 一切正常。
4. 无分配器 Env 的 `threadCpuLeaseProviderFor<Env>`：`cpu=4` 抛 `ThreadCpuBindError`，`none` 正常。
5. 重复设置 provider：第二次调用 `xxx_setThreadCpuLease` 抛 `std::logic_error`，原 provider 继续生效。
6. hwloc Env（`TEST_HAS_HWLOC`）：`installThreadCpuLeaseProviders<Env>` 后 `cpu=auto` 的 shm_seqlock 读线程与
   一个 `CpuAutoBind()` 图节点落在不同物理核；两个 transport 线程显式绑同一核抛 `CpuAutoBindError`；
   aeron `driverCpu` 与 `cpu` 共存。
7. 共享线程：同一 identifier 两个 locator 不同 `cpu`，首个生效、第二个只警告；`senderCpu` 同理。
8. 模板路径：通过 `XxxImporterExporter<Env>::createImporter` 走 RealTimeApp，不手工安装 provider，`cpu=auto` 生效。
9. 稳定性：绑核线程反复 subscribe/unsubscribe（复用 `ipc_concurrency_test` 的 churn 场景并加 `cpu` 属性）。

回归：`shm_seqlock_test`、`shm_cursor_test`、`zeromq_test`、`multicast_test`、`aeron_ipc_test`、
`ipc_concurrency_test`、`multitransport_lifecycle_test`、`threaded_handler_transport_test` 全部重跑。
性能参考（不作验收）：`ipc_latency_profiler` 对 shm_seqlock 跑 `cpu=` 内部绑核与外部 `taskset` 的对比，
确认数字一致。

## 构建

`cd build-common && source ./build-env-setting.sh`；在 tm_transport worktree 的 `builddir_release` /
`builddir_debug` 中按目标定向编译（`ninja -j2`），最后 release + debug 全量构建并安装到开发 `install/`；
tm_examples debug 清理重建一次确认下游编译。tm_infra / tm_basic 不参与改动。

## 文档与 git

- 分支：tm_transport `feature/ipc-thread-affinity`（worktree `worktrees/tm_transport-ipc-thread-affinity`，
  起点 `develop` `e22df3c`），完成后合并回 `develop`，不动 master。
- 任务记录：本目录 `README.md`，完成后在 `doc/INDEX.md` 更新状态。
- tm_kit_reference（推送后）：`tm_transport/03-transports/{shm_seqlock,shm_cursor,aeron_ipc,shared_memory_broadcast,zeromq,multicast}.md`
  加 `cpu` 属性；`00-overview.md` 增加“transport 线程绑核”一节（`xxx_setThreadCpuLease` /
  `installThreadCpuLeaseProviders`）与“CPU 列表不能用逗号”的提示；`tm_infra/10-realtime-executors.md`
  自动绑核一节加一句指向。
