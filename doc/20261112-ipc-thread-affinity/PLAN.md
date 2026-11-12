# 方案（v3）：IPC transports 内部线程通过 ConnectionLocator 绑核，经 `xxx_setThreadCpuLease` 复用 infra 的 ThreadCpuLease

## 修订说明

- v1：显式 CPU 列表 + 直接 `pthread_setaffinity_np`，不复用 infra。用户否决（要求复用 infra/basic 绑核设施）。
- v2：tm_infra 增加进程级默认分配器登记点，transport 在调用线程里 acquire、工作线程里 pin。需要改 tm_infra，且引入全局状态。
- **v3（用户提议，本版采纳）**：每个有内部线程的 transport 组件增加一个接口
  `xxx_setThreadCpuLease(provider)`，`provider` 是一个返回 `infra::ThreadCpuLease` 的可调用对象，
  参数为 **线程描述字符串** 与 **绑核 spec 字符串**（和 `ThreadCpuLease::bindCurrentThreadBy` 的参数一致）。
  transport 在自己的线程里调用 provider 取得租约并持有到线程结束。
  - **tm_infra / tm_basic 不需要改动**；绑核语义完全由 provider（通常就是 `bindCurrentThreadBy<Env>`）决定。
  - 没有全局状态；Env 带分配器时自动/显式绑核都走分配器，没有分配器时与 infra 的既定行为一致（除 `none` 外抛异常）。
  - 测试可以注入 Fake 分配器的 provider，不依赖 hwloc。

## 现状与约束（探索结论，不变）

- tm_infra `RealTimeThreadedExecutor.hpp` 已有：`CpuBindingSpec::parse`（`none`/`auto`/`numa:<n>`/`cpus:<list>`/`<list>`），
  `IThreadCpuAllocator`，`ThreadCpuAllocatorComponent<Access>`，`ThreadCpuLease`（`bindCurrentThreadBy<Env>(spec, who)`
  以及接收 `IThreadCpuAllocator &` 的重载；只绑**调用线程**；每线程同时只能持一个租约；可移动不可拷贝）。
  `bindCurrentThreadBy<Env>`：`none`/空返回未激活租约；Env 无分配器时其它 spec 抛 `ThreadCpuBindError`。
- tm_basic `HwlocThreadCpuAllocator`：显式 `reserve` 按物理核独占（含 SMT 兄弟、excludeCpus），冲突抛 `CpuAutoBindError`。
- tm_transport 组件是非模板 pimpl，内部看不到 Env；所有知道 Env 的调用点（下列）都能安装 provider：
  - 各 `tm_kit/transport/*/XxxImporterExporter.hpp`：`createImporter` / `createTypedImporter` / `createExporter` /
    `createTypedExporter` 的 `start(env)`，以及文件尾部 `*id = env->xxx_addSubscriptionClient(...)` 的两个直接订阅辅助函数；
  - `MultiTransportBroadcastListener.hpp`（:425-980，`component` 由 `env` 转换而来）；
  - `HeartbeatAndAlertComponent.hpp`（:123-296，`xxx_getPublisher`，影响 multicast/singlecast 的发送线程）。
- aeron_ipc 现有私有 `parseCpuList`/`bindCurrentThread`（`driverCpu`，只绑内嵌 media driver 的 agent 线程，通过
  `aeron_driver_context_set_agent_on_start_function` 在 Aeron 自己的线程里回调，C 回调不能抛异常）；
  driver 在 `closeEmbeddedDriver()`（:325）关闭，Aeron 内部 join agent 线程。
- ConnectionLocator 属性值不能含 `,`，CPU 列表用 `2-5` / `2:5` 或单个数字。
- zeromq / nng 在建线程前把 locator 裁成 host/port/identifier，属性会丢，需先解析再裁剪；
  singlecast / shared_memory_broadcast 的线程 detach 后句柄不可靠。**统一在线程函数第一行自绑**（`ThreadCpuLease` 本来就只绑调用线程）。
- shm_seqlock 读线程在 `addSubscription` 持 `mutex_` 时懒启动；`run()` 第一步是 `openAndJoin()`，绑核放在它之前，不需要 `mutex_`，
  因此启动握手可以在持锁状态下等待。

## 一、公共类型（tm_transport，新增 `tm_kit/transport/ThreadCpuLeaseProvider.hpp`，纯头文件）

```cpp
namespace dev::cd606::tm::transport {
    // (who, spec) -> 租约。who 形如 "shm_seqlock reader feed"、"multicast sender"、"aeron_ipc driver conductor"；
    // spec 为 locator 属性值原文（CpuBindingSpec 语法）。在要绑核的线程里调用；抛出的异常由 transport 处理（见下）。
    using ThreadCpuLeaseProvider = std::function<infra::ThreadCpuLease(std::string const &who, std::string const &spec)>;

    // 常用 provider：直接转 ThreadCpuLease::bindCurrentThreadBy<Env>。
    template <class Env>
    ThreadCpuLeaseProvider threadCpuLeaseProviderFor() {
        return [](std::string const &who, std::string const &spec) {
            return infra::ThreadCpuLease::bindCurrentThreadBy<Env>(spec, who);
        };
    }
    // 给 Env 里存在的每个支持绑核的 transport 组件安装上面的 provider（已有 provider 的组件不覆盖）。
    // 结构同 networkTransportComponents_threadHandles：对每个组件 if constexpr (is_convertible_v<Env*, Xxx*>)。
    template <class Env>
    void installThreadCpuLeaseProviders(Env *env);
}
```

组件接口（每个纳入范围的组件各加一个，命名沿用前缀风格）：
```cpp
// 设置 provider，只能设置一次。已设置时抛 std::logic_error（不支持更换 provider，
// 也不提供清除接口）。只影响之后创建的线程；已在运行的线程不受影响。
// 线程安全（组件 mutex 保护）。
void shm_seqlock_setThreadCpuLease(ThreadCpuLeaseProvider provider);
```
因为 provider 不可更换，**用户自定义 provider 必须在图启动前设置**（即在 `AppRunner` 运行、任何 importer/exporter
`start(env)` 之前）。否则框架自动安装的默认 provider 已经占位，后续调用抛 `std::logic_error`。

框架内部的自动安装走组件的私有入口，语义是“没装才装”，不报错、可被重复调用：
```cpp
private:
    // 由 installThreadCpuLeaseProviders<Env> 调用；已有 provider 时保持原样。
    void shm_seqlock_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider);
    template <class E> friend void installThreadCpuLeaseProviders(E *);
```
这样两类语义分开，公开 API 上不出现 bool：手工设置是“一次且唯一”，自动安装是幂等的。

`installThreadCpuLeaseProviders<Env>` 的调用点：上面列出的所有知道 Env 的地方，在调用 `xxx_addSubscriptionClient` /
`xxx_getPublisher` 之前调用一次（幂等，开销是一次组件 mutex + 判空）。直接调组件 API 的代码自己调
`env->xxx_setThreadCpuLease(threadCpuLeaseProviderFor<Env>())` 或传自定义 lambda（例如绑到测试用 Fake 分配器：
`[&](auto const &who, auto const &spec){ return infra::ThreadCpuLease::bindCurrentThreadBy(fakeAllocator, spec, who); }`），
且必须在第一个订阅/发布建立之前完成。

## 二、transport 内部统一流程（放在 `src/ThreadCpuLeaseSupport.hpp`，src 内部头，不安装）

```cpp
// 在创建线程的调用线程里：读属性、只做语法校验（CpuBindingSpec::parse，失败抛 std::invalid_argument，
// 信息含 transport 名、属性名、值、locator，与 zeromq hwm 一致）。
// spec 非 none 且组件没有 provider -> 抛 std::invalid_argument（提示调用 xxx_setThreadCpuLease 或 installThreadCpuLeaseProviders）。
struct ThreadCpuBindRequest { std::string who; std::string spec; ThreadCpuLeaseProvider provider; bool active() const; };
ThreadCpuBindRequest makeThreadCpuBindRequest(ConnectionLocator const &, std::string const &property, std::string const &who,
                                              ThreadCpuLeaseProvider const &providerOrEmpty, char const *transportName);

// 在新线程第一行：调 provider 取租约，把结果（成功 / exception_ptr）通过 promise 交回；租约作为线程函数局部变量持有，
// 线程退出时析构 -> 归还 CPU。
class ThreadStartHandshake { std::promise<void> p_; std::future<void> f_; ... };
```
- **错误同步化**：创建线程后，调用线程 `future.get()` 等待工作线程完成绑核；工作线程绑核失败时把异常放进 promise 并直接退出
  （不进入收发循环）；调用线程收到异常后清理（join 线程、从缓存 map 删除订阅对象、不留下半启动状态）并把异常原样抛给
  `addSubscriptionClient` / `getPublisher` 的调用者。异常类型由 provider 决定（`ThreadCpuBindError` / `CpuAutoBindError` /
  `std::invalid_argument`），不包装。
- 不带 `cpu` 属性或 `none`：不调 provider，不做握手，行为与现在完全一致（零开销路径）。
- 共享线程：按 host/port/identifier 共享的订阅线程，**首个创建者的 `cpu` 生效**；后续不同值只打一条 stderr 警告
  （与 zeromq HWM 一致）。aeron_ipc 按完整 locator 分线程，不受此限。
- 非 Linux：`bindCurrentThreadBy` 自己抛 `ThreadCpuBindError`，同步到调用者。

## 三、范围与属性

| 档 | transport | 线程 | 属性 | 新接口 |
|---|---|---|---|---|
| 1 共享内存 IPC | shm_seqlock | 每 identifier 一个读线程（懒启动） | `cpu` | `shm_seqlock_setThreadCpuLease` |
| | shm_cursor | 每 identifier 一个读线程 | `cpu` | `shm_cursor_setThreadCpuLease` |
| | shared_memory_broadcast | 每 identifier 一个接收线程（detached） | `cpu` | `shared_memory_broadcast_setThreadCpuLease` |
| | aeron_ipc | 订阅 poll 线程（每完整 locator 一个）；内嵌 driver agent 线程 | `cpu`；`driverCpu` | `aeron_ipc_setThreadCpuLease` |
| 2 unix socket IPC | zeromq | 每 host+port+identifier 一个接收线程 | `cpu` | `zeroMQ_setThreadCpuLease` |
| | nng | 同上 | `cpu` | `nng_setThreadCpuLease` |
| 3 loopback 可当 IPC | multicast | 每 host:port 接收线程；组件级共享发送线程 | `cpu` / `senderCpu` | `multicast_setThreadCpuLease` |
| | singlecast | 同上 | `cpu` / `senderCpu` | `singlecast_setThreadCpuLease` |

不在范围：redis / rabbitmq / nats / websocket / grpc / socket_rpc（纯网络），lock_free_in_memory_shared_chain（无内部线程）。

属性语义：
- `cpu=<spec>`：`CpuBindingSpec` 语法，`none`/空（默认）、`auto`、`numa:<n>`、`cpus:<list>`、`<list>`（`4`、`2-5`、`2:5`）。
  绑定效果 = `provider(who, spec)`：Env 带 `HwlocThreadCpuAllocatorComponent` 时 `auto`/`numa:` 自动选核、显式列表 reserve 独占；
  Env 无分配器时除 `none` 外抛 `ThreadCpuBindError`（infra 既定语义，本任务不改）。
- `senderCpu=<spec>`（multicast / singlecast）：组件级共享发送线程，首个 `getOrStartSender` 的值生效，后续只警告。
- `driverCpu=<spec>`（aeron）：语法升级为同一 spec（旧写法 `2`、`2-3`、`2:3` 仍合法）；`TM_AERON_IPC_DRIVER_CPU` 回退保留。
  每个 agent 线程（shared 模式 1 个，dedicated 模式 3 个）在 `onDriverAgentStart` 里各调一次 provider，`who` 带 Aeron 的 roleName；
  租约存在 publisher 对象的 `std::vector<ThreadCpuLease>`（mutex 保护），`closeEmbeddedDriver()` 在 `aeron_driver_close` 之后清空。
  **C 回调不能抛异常**：失败时打 stderr 并记录到 `AeronIpcPublisherStats` 新增字段 `driverBindFailed`，driver 继续运行
  （与现有 `driverCpu` 失败只打 stderr 的行为一致）。`auto` 下 dedicated 模式会占 3 个核，头文件注释写明。

示例：
```
shm_seqlock://::::feed[wait=spin,cpu=4]
shm_cursor://::::feed[slotSize=512,cpu=cpus:4-5]
shm_seqlock://::::feed[cpu=auto]                 # Env 需带 basic::HwlocThreadCpuAllocatorComponent
aeron_ipc://::::feed[driverCpu=2,cpu=numa:0]
zeromq://ipc:0:::/tmp/feed[rcvhwm=0,cpu=6]
multicast://239.0.0.1:12345[interface=lo,cpu=6,senderCpu=8]
```

## 四、各文件改动

共用：
- 新增 `tm_kit/transport/ThreadCpuLeaseProvider.hpp`（安装，登记到 `tm_kit/transport/meson.build`）；
  `src/ThreadCpuLeaseSupport.hpp`（内部）。无新 .cpp。
- 各 `XxxComponent.hpp`：加 `xxx_setThreadCpuLease`；类注释补 `cpu`（及 `senderCpu`/`driverCpu`）说明与“需要 provider”的提示。
- 各 `XxxImporterExporter.hpp`：`start(env)` 与直接订阅辅助函数里先 `installThreadCpuLeaseProviders<Env>(env)`。
- `MultiTransportBroadcastListener.hpp`：在节点 `start(Env *env)` 里安装一次（见下节）。`HeartbeatAndAlertComponent.hpp` 不改：
  它的 `xxx_getPublisher` 只出现在 `HeartbeatAndAlertComponentInitializer` 里，且被 `std::bind` 包装，发送线程由 ImporterExporter
  之外的路径创建 —— 实际上它用的是组件自己的 `getPublisher`，而 provider 已由 Env 统一安装时不需要单独处理；provider 未安装时
  `senderCpu` 会按“无 provider + 非 none”抛异常，这是预期行为。

### MultiTransportBroadcastListenerManagingUtils / MultiTransportBroadcastPublisherManagingUtils：不需要改动

两个 ManagingUtils 自身不直接调用 transport 组件 API，所有路径都收敛到下层入口：

| 文件 / 函数 | 下层入口 | provider 来源 |
|---|---|---|
| `MultiTransportBroadcastListenerManagingUtils::setupOneBroadcastListener_internal`（:63-308） | `XxxImporterExporter<Env>::create[Typed]Importer` | ImporterExporter `start(env)` |
| `::oneByteDataBroadcastListener`（:532-710） | 同上 | 同上 |
| `MultiTransportBroadcastFirstUpdateQueryManagingUtils::fetch[Typed]FirstUpdateAndDisconnect`（:884-1050） | `XxxImporterExporter<Env>::fetchFirstUpdateAndDisconnect(env, ...)` | 该静态辅助函数里安装 |
| `::setupBroadcastListener[WithTopic]ThroughHeartbeat`（:772-872） | 构造 `MultiTransportBroadcastListener<Env,T>` 节点（:813） | 见下节（节点自身安装） |
| `MultiTransportBroadcastPublisherManagingUtils::one[ByteData]BroadcastPublisher`（:35-381，含 `WithProtocol`） | `XxxImporterExporter<Env>::create[Typed]Exporter` | Exporter `start(env)` |

其中 multicast / singlecast 的**发送线程是组件级共享**的，由 `getPublisher` 触发创建；`start(env)` 先安装 provider 再调
`getPublisher`，`senderCpu` 在首次创建发送线程时读取，顺序正确。因此这两个文件只要确认“所有调用都走 ImporterExporter”
这一前提成立，**代码零改动**；在文档里记录该前提，后续若有人在这两个文件里新增直接调用组件 API 的路径，需要自行安装 provider。

### MultiTransportBroadcastListener.hpp：节点自身安装 provider（必须改）

`MultiTransportBroadcastListener<Env,T>` 是独立节点类，`actuallyHandle` 里**绕过 ImporterExporter 直接调组件 API**：
AddSubscription 分支对 11 种 transport 各一次 `static_cast<XxxComponent *>(env)->xxx_addSubscriptionClient(...)`
（:420-980），RemoveAllSubscriptions 分支各一次 remove（:1186-1273）。改动：

1. 在节点的 `start(Env *env)`（`AbstractIntegratedOnOrderFacilityWithExternalEffects` 的生命周期入口）里调用一次
   `installThreadCpuLeaseProviders<Env>(env)`；走 `IfUnset` 语义，重复 start 无副作用，也不会覆盖用户预先设置的 provider。
2. `restart` 流程（`restartInfo_` 在重新 start 后重放 AddSubscription）复用 `actuallyHandle` 的同一路径，自动带上 provider。
3. remove 路径不涉及 provider：订阅线程 join 后租约在线程函数退出时析构归还；`RemoveAllSubscriptions` 清空 `subscriptions_`
   之后线程已 join，不会出现“线程还活着但租约已归还”。
4. `fetchFirstUpdateAndDisconnect` 的延迟 10ms 删除线程（各 ImporterExporter 里）与此无关：租约跟着订阅线程走，线程结束即归还。

`MultiTransportFacilityWrapper` / `MultiTransportRemoteFacility` / `SyntheticMultiTransportFacility` 等上层工具最终都经由
上述节点或 `XxxImporterExporter`，没有第三类绕过路径（已 grep 确认 `_addSubscriptionClient(` / `_getPublisher(` 的调用点全集）。

各 transport：
- `src/shm_seqlock/ShmSeqlockComponent.cpp`：Impl 存 provider；`OneShmSeqlockSubscription` 构造时 `makeThreadCpuBindRequest`；
  `addSubscription` 懒启动线程处加握手（持 `mutex_` 等待，`run()` 第一行绑核，在 `openAndJoin()` 之前）；失败则 `Impl::addSubscriptionClient`
  删除刚建的订阅并抛出；后续 locator `cpu` 不同只警告。
- `src/shm_cursor/ShmCursorComponent.cpp`：`ShmCursorOptions` 加 `cpuSpec`；`ShmCursorSubscription` 构造接收 request，线程 lambda 第一行绑核 + 握手。
- `src/shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp`：`getOrStartSubscription` 解析传入构造；`run()` 第一行绑核；握手在 `detach()` 之前完成。
- `src/aeron_ipc/AeronIpcComponent.cpp`：删私有 `parseCpuList`/`bindCurrentThread`；`driverCpu` 走 provider（见上）；
  `AeronIpcSubscriber` 构造解析 `cpu`，`run()` 第一行绑核 + 握手。
- `src/zeromq/ZeroMQComponent.cpp`、`src/nng/NNGComponent.cpp`：`getOrStartSubscription` 在裁剪 locator 前解析（与 `rcvhwm` 同处），
  作为构造参数传入，`run()` 第一行绑核 + 握手。
- `src/multicast/MulticastComponent.cpp`、`src/singlecast/SinglecastComponent.cpp`：接收 io_context 线程绑 `cpu`；共享发送线程绑 `senderCpu`
  （首个生效）；singlecast 接收线程改为在 lambda 内自绑（不再依赖 detach 后的句柄）。

不做的事：不改 tm_infra / tm_basic；不改 ConnectionLocator 解析规则；不改 `*_threadHandles()`；
`xxx_setThreadCpuLease` 是唯一新增的组件公共 API，不设置时行为与现在完全一致（向后兼容）。

## 五、测试：tm_transport 新增 `test/ipc_thread_affinity_test/`

`IpcThreadAffinityTest.cpp` + `meson.build`（`shm_cursor_test` 模式，`-lrt`、`protobuf`；`dependency('hwloc', required: false)`
可用时加 `-DTEST_HAS_HWLOC`）+ `run_tests.sh`；在 `test/meson.build` linux 块注册。
验证：订阅建立后扫描 `/proc/self/task/*/status` 的 `Cpus_allowed_list`，断言预期线程绑到指定列表、其余线程掩码不变；收发消息确认功能正常。
Fake 分配器从 `tm_infra/test/threaded_handler_test/ThreadedNodeOptionsTest.cpp:177` 复制一份。

用例：
1. Fake 分配器 provider：每个 transport（档 1-3，aeron 条件编译）`cpu=auto` 调 acquire 并绑到返回的 CPU；`cpus:4` / `4-5` 调 reserve 并绑定；
   `cpus:` 前缀、`none`、空值；取消订阅、线程结束后 release 被调用（租约随线程析构）。
2. 同步失败：provider 抛异常时 `addSubscriptionClient` / `getPublisher` 原样抛出同类型异常，之后无残留线程、订阅缓存为空、
   再次订阅（不带 `cpu`）正常；语法错误（`abc`、`-1`）抛 `std::invalid_argument`。
3. 无 provider：`cpu=4` 抛 `std::invalid_argument`（信息含接口名）；不带 `cpu` 一切正常。
4. 无分配器 Env 的 `threadCpuLeaseProviderFor<Env>`：`cpu=4` 抛 `ThreadCpuBindError`，`none` 正常。
5. hwloc Env（`TEST_HAS_HWLOC`）：`installThreadCpuLeaseProviders<Env>` 后 `cpu=auto` 的 shm_seqlock 读线程与一个 `CpuAutoBind()` 图节点落在不同物理核；
   两个 transport 线程显式绑同一核抛 `CpuAutoBindError`；aeron `driverCpu` 与 `cpu` 共存。
6. 共享线程：同一 identifier 两个 locator 不同 `cpu`，首个生效、第二个只警告；`senderCpu` 同理。
7. 模板路径：通过 `ShmSeqlockImporterExporter<Env>::createImporter` 走 RealTimeApp，不手工安装 provider，`cpu=auto` 生效。
8. 稳定性：绑核线程反复 subscribe/unsubscribe（复用 `ipc_concurrency_test` churn 场景加 `cpu` 属性）。

回归：`shm_seqlock_test`、`shm_cursor_test`、`zeromq_test`、`multicast_test`、`aeron_ipc_test`、`ipc_concurrency_test`、
`multitransport_lifecycle_test`、`threaded_handler_transport_test` 全部重跑。
性能参考（不作验收）：`ipc_latency_profiler` 对 shm_seqlock 跑 `cpu=` 内部绑核 vs 外部 `taskset`，确认数字一致。

## 六、构建
`cd build-common && source ./build-env-setting.sh`；tm_transport 在已有 `builddir_release` / `builddir_debug` 中
`ninja -j2 -C builddir_release <目标>` 定向编译测试；最后 release + debug `./build.sh ../tm_transport` 安装到 `install/`；
tm_examples debug 清理重建一次确认下游编译。tm_infra / tm_basic 不动。

## 七、文档与 git
- 分支：tm_transport `feature/ipc-thread-affinity`（已建，基于 develop e22df3c，尚无改动）；完成后合并回 develop，不碰 master。
- 任务记录：`tm_transport/doc/20261112-ipc-thread-affinity/README.md`，`doc/INDEX.md` 加一条；关联 `20261105-zeromq-hwm-locator`、
  `20261103-ipc-concurrency-test`，以及 `tm_infra/doc/20261111-thread-cpu-autobind`（ThreadCpuLease 来源，在其“后续任务”加链接）。
- tm_kit_reference（推送后）：`tm_transport/03-transports/{shm_seqlock,shm_cursor,aeron_ipc,shared_memory_broadcast,zeromq,multicast}.md`
  加 `cpu` 属性；`00-overview.md` 加“transport 线程绑核：`xxx_setThreadCpuLease` / `installThreadCpuLeaseProviders`”一节与
  “CPU 列表不能用逗号”提示；`tm_infra/10-realtime-executors.md` 自动绑核一节加一句指向。
- 提交：`feat(transport): ...`、`test:`、`doc:`，结尾 Co-Authored-By 行。

## 需要用户确认的点
1. ~~provider 是否自动安装、能否更换~~ **已确认（2026-11-12）**：不支持重新设置 provider。公开接口
   `xxx_setThreadCpuLease(provider)` 只能设置一次，已设置时抛 `std::logic_error`；框架自带的自动安装走私有
   `IfUnset` 语义，重复调用无副作用。因此用户自定义 provider 必须在图启动前设置。
2. **没有 provider 却写了 `cpu=`**：抛 `std::invalid_argument` 提示安装接口（推荐，错误不静默）。备选：显式列表退化为直接 pin 不走分配器。
3. provider 在工作线程里调用，失败通过启动握手**同步**抛回 `addSubscriptionClient` / `getPublisher`；aeron driver agent 线程例外（C 回调，只打 stderr + 统计字段）。
4. 第 3 档 multicast / singlecast 纳入；属性名 `cpu` / `senderCpu` / `driverCpu`。
