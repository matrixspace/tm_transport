# tm_transport 任务文档索引

## 任务列表

- [IPC Transport 并发测试](20261103-ipc-concurrency-test/README.md) — 2026-11-03，为 shm_seqlock、shm_cursor、aeron_ipc、shared_memory_broadcast 四种 IPC transport 新增系统性并发测试程序，覆盖进程内多线程读写、多进程读写、graph 场景等 10 类测试。

- [IPC Transport 并发 Bug 修复](20261103-ipc-concurrency-bugfix/README.md) — 2026-11-03 至 2026-11-04，基于并发测试结果修复三类 bug：shm_seqlock 计数器数据竞争、shared_memory_broadcast publisher 侧 SIGSEGV、aeron_ipc untethered 读端 term 重用导致数据损坏。

- [ZeroMQ 通过 ConnectionLocator 配置 HWM](20261105-zeromq-hwm-locator/README.md) — 2026-11-05，ZeroMQ 传输支持通过 locator 属性 `hwm`/`sndhwm`/`rcvhwm` 配置高水位，非法值抛 `std::invalid_argument`。

- [IPC Transport 内部线程绑核](20261112-ipc-thread-affinity/README.md) — 2026-11-12 起，进行中：各 IPC transport 组件新增 `xxx_setThreadCpuLease` 注入接口，locator 属性 `cpu`/`senderCpu`/`driverCpu` 经 provider 复用 infra 的 `ThreadCpuLease` 绑核。

## 任务关系

并发测试任务（20261103-ipc-concurrency-test）发现了并发 Bug，由并发 Bug 修复任务（20261103-ipc-concurrency-bugfix）解决。两个任务文档互相关联。

- [RealTimeApp threaded handler 执行器优化（主记录在 tm_infra）](../../tm_infra/doc/20261105-realtime-threaded-executor/README.md) — 2026-11-05 起，进行中：第二阶段执行器抽象重构。tm_infra 的 ThreadedHandlerBase 改为可替换执行器；本仓库新增 `test/threaded_handler_transport_test`，以 JsonRESTClientFacility（直接继承 ThreadedHandler）在默认执行器与共享线程池执行器下做端到端回归。


- 2026-11-09：[执行器实现分层](../../tm_infra/doc/20261105-realtime-threaded-executor/executor-layering.md)：共享池接入测试切换到basic头文件和命名空间。
