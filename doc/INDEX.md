# tm_transport 任务文档索引

## 任务列表

- [ShmSeqlockComponent 系统测试](20261113-shm-seqlock-system-test/README.md) — 2026-11-13 起，功能/异常验收完成：core 778/778、夜间重复 15,560/15,560、图 192/192、Heartbeat 30 PASS+3 风险探测、基线 4/4；资源 18/19（DEF-1 已知限制，不修复）。性能完成 staged 基线、饱和、50%/90% 和全量 1200 配置矩阵（零丢失）。2026-11-14 09:09 至 11-16 09:50 完成 10 lane 多路稳定性（48.7h，用户确认时长足够后停止）：10/10 PASS，总发送 37.67B 条，corrupt/dup/ooo 全 0，资源无增长（ΔRSS/FD/线程/映射=0）。跨 NUMA 补充：性能 80 轮全部 PASS（跨节点 spin p50 +36–52%、饱和吞吐 −34%），1h 稳定性 4/4 lane PASS、零丢失、资源零增长。图/MultiTransport 性能 60 轮全部 PASS：10k 条/s 零丢失，饱和时 1 监听端 0.52–0.59M 条/s、4 监听端有损（按设计）。单配置 2h/24h soak、推送与参考文档同步未执行。**测试报告：** [REPORT.md](20261113-shm-seqlock-system-test/REPORT.md)（功能、效率、稳定性，含未覆盖场景清单）。

- [ShmSeqlockComponent 默认参数调整](20261116-shm-seqlock-defaults/README.md) — 2026-11-16 完成：基于上述系统测试报告把默认 `slotSize` 1000→2048、`layout` inline→inline-premark、`writerLock` false→true（显式 `writerLock=false` 仍可强制关锁）。新增 `shm_seqlock_test defaults` 回归验证默认值；并发测试"无锁"探针改为显式关锁，保住未加锁路径的覆盖。

- [ShmSeqlockComponent 读端 join 默认改为 next](20261116-shm-seqlock-join-default/README.md) — 2026-11-16 起：默认 `join` latest→next（只交付加入之后发布的消息），fetch-first/心跳发现随默认变 next；`defaults` 回归新增 join=next 校验。

- [IPC Transport 并发测试](20261103-ipc-concurrency-test/README.md) — 2026-11-03，为 shm_seqlock、shm_cursor、aeron_ipc、shared_memory_broadcast 四种 IPC transport 新增系统性并发测试程序，覆盖进程内多线程读写、多进程读写、graph 场景等 10 类测试。

- [IPC Transport 并发 Bug 修复](20261103-ipc-concurrency-bugfix/README.md) — 2026-11-03 至 2026-11-04，基于并发测试结果修复三类 bug：shm_seqlock 计数器数据竞争、shared_memory_broadcast publisher 侧 SIGSEGV、aeron_ipc untethered 读端 term 重用导致数据损坏。

- [ZeroMQ 通过 ConnectionLocator 配置 HWM](20261105-zeromq-hwm-locator/README.md) — 2026-11-05，ZeroMQ 传输支持通过 locator 属性 `hwm`/`sndhwm`/`rcvhwm` 配置高水位，非法值抛 `std::invalid_argument`。

- [IPC Transport 内部线程绑核](20261112-ipc-thread-affinity/README.md) — 2026-11-12 至 2026-11-13 完成：各 IPC transport 组件新增 `xxx_setThreadCpuLease` 注入接口，locator 属性 `cpu`/`senderCpu`/`driverCpu` 经 provider 复用 infra 的 `ThreadCpuLease` 绑核；新增 `test/ipc_thread_affinity_test`（287 项检查全通过），并顺带修复 singlecast/smb detach 后句柄失效与 smb 取消订阅的 use-after-free。

- [aeron_ipc untethered 超时默认值改回 10ms](20261113-aeron-untethered-defaults/README.md) — 2026-11-13 完成：`05962f6` 把 `untetheredWindowLimitTimeoutMs`/`untetheredLingerTimeoutMs` 默认值改为 2s，导致一个慢读者让发布者在 `backPressure=drop` 下周期性丢光所有读者的消息（aeron test3 失败）；默认值改回 10ms，正确性由既有的帧头复检（`overrun`）保证。

## 任务关系

并发测试任务（20261103-ipc-concurrency-test）发现了并发 Bug，由并发 Bug 修复任务（20261103-ipc-concurrency-bugfix）解决。两个任务文档互相关联。

- [RealTimeApp threaded handler 执行器优化（主记录在 tm_infra）](../../tm_infra/doc/20261105-realtime-threaded-executor/README.md) — 2026-11-05 起，进行中：第二阶段执行器抽象重构。tm_infra 的 ThreadedHandlerBase 改为可替换执行器；本仓库新增 `test/threaded_handler_transport_test`，以 JsonRESTClientFacility（直接继承 ThreadedHandler）在默认执行器与共享线程池执行器下做端到端回归。


- 2026-11-09：[执行器实现分层](../../tm_infra/doc/20261105-realtime-threaded-executor/executor-layering.md)：共享池接入测试切换到basic头文件和命名空间。
