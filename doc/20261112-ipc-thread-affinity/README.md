# IPC Transport 内部线程通过 ConnectionLocator 绑核

**任务开始时间：** 2026-11-12  
**任务结束时间：**  
**状态：** 进行中  
**分支：** `feature/ipc-thread-affinity`（worktree `worktrees/tm_transport-ipc-thread-affinity`，起点 commit `e22df3c`）

## 背景

`shm_seqlock`、`shm_cursor`、`shared_memory_broadcast`、`aeron_ipc`、`zeromq(ipc)`、`nng(ipc)` 等 IPC transport
在组件内部各自创建接收/轮询线程，`multicast` / `singlecast` 还有组件级共享发送线程。这些线程目前只能由外部
`taskset` / `numactl` 整体约束进程，无法单独分配到核心，低延迟部署时不能把 hot thread 与框架/杂务线程分开。

同时 tm_infra 已经有一套完整的绑核设施（`CpuBindingSpec`、`IThreadCpuAllocator`、`ThreadCpuLease`、
`ThreadCpuAllocatorComponent`），tm_basic 有 hwloc 实现（`HwlocThreadCpuAllocator`，含 SMT/NUMA 感知与显式独占）。
本任务让 transport 内部线程复用这套设施，而不是另造一套 `pthread_setaffinity_np`。

## 目标

1. 每个有内部线程的 IPC transport 组件新增一个注入接口
   `xxx_setThreadCpuLease(ThreadCpuLeaseProvider, bool replaceExisting = true)`，
   provider 签名为 `(who, spec) -> infra::ThreadCpuLease`，与 `ThreadCpuLease::bindCurrentThreadBy` 一致。
2. ConnectionLocator 新增属性 `cpu`（接收/轮询线程）、`senderCpu`（multicast/singlecast 共享发送线程）、
   `driverCpu`（aeron 内嵌 driver，语法升级但保持旧写法兼容）。属性值使用 `CpuBindingSpec` 语法。
3. 各 `XxxImporterExporter` 模板、`MultiTransportBroadcastListener` 节点、`fetchFirstUpdateAndDisconnect` 等
   知道 `Env` 的入口，幂等安装默认 provider（`threadCpuLeaseProviderFor<Env>()`）。
4. tm_infra / tm_basic **不改动**；不设置 provider 时行为与现在完全一致（向后兼容）。

## 方案

完整方案见 [PLAN.md](PLAN.md)（v3）。关键决策：

- 绑核发生在工作线程自身（`ThreadCpuLease` 只绑调用线程），线程函数第一行调用 provider，租约作为线程函数局部变量
  持有，线程退出即归还。
- provider 在工作线程内抛出的异常通过启动握手（promise/future）同步交回 `addSubscriptionClient` / `getPublisher`
  的调用者，避免出现“订阅已经返回、线程却没起来”的半启动状态。
- 共享线程（同一 host/port/identifier 复用）首个 locator 的 `cpu` 生效，后续不同值只打 stderr 警告。
- aeron 内嵌 driver 的 agent 线程由 Aeron 创建并经 C 回调启动，不能抛异常：失败打 stderr 并计入
  `AeronIpcPublisherStats::driverBindFailed`。

## 待确认

方案 [PLAN.md](PLAN.md) 末尾列出 4 个待确认点（自动安装策略、无 provider 时的错误行为、启动握手、
第 3 档范围与属性命名）。

## 代码改动（按文件列出）

（实现阶段补全）

## 测试与验证（命令、构建类型、结果）

（实现阶段补全）

## 兼容性与注意事项

（实现阶段补全）

## 相关文档

- [方案 v3](PLAN.md)
- 前序：[ZeroMQ 通过 ConnectionLocator 配置 HWM](../20261105-zeromq-hwm-locator/README.md)（同为 locator 属性扩展）
- 前序：[IPC Transport 并发测试](../20261103-ipc-concurrency-test/README.md)（各 transport 线程模型）
- 设施来源：[节点线程自动绑核 CpuAutoBind](../../../tm_infra/doc/20261111-thread-cpu-autobind/README.md)（`ThreadCpuLease`）
