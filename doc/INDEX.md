# tm_transport 任务文档索引

## 任务列表

- [IPC Transport 并发测试](20261103-ipc-concurrency-test/README.md) — 2026-11-03，为 shm_seqlock、shm_cursor、aeron_ipc、shared_memory_broadcast 四种 IPC transport 新增系统性并发测试程序，覆盖进程内多线程读写、多进程读写、graph 场景等 10 类测试。

- [IPC Transport 并发 Bug 修复](20261103-ipc-concurrency-bugfix/README.md) — 2026-11-03 至 2026-11-04，基于并发测试结果修复三类 bug：shm_seqlock 计数器数据竞争、shared_memory_broadcast publisher 侧 SIGSEGV、aeron_ipc untethered 读端 term 重用导致数据损坏。

## 任务关系

并发测试任务（20261103-ipc-concurrency-test）发现了并发 Bug，由并发 Bug 修复任务（20261103-ipc-concurrency-bugfix）解决。两个任务文档互相关联。
