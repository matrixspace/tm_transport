# IPC Transport 并发 Bug 修复

**任务开始时间：** 2026-11-03  
**任务结束时间：** 2026-11-04  
**状态：** 完成  
**分支：** `feature/ipc-concurrency-bugfix`（已合并到 `develop`，commit `77d0008`）

## 背景

基于之前对 shm_seqlock、shm_cursor、aeron_ipc 多线程发布风险的分析（见[并发测试任务](../20261103-ipc-concurrency-test/README.md)），对四种 IPC transport 进行了系统性并发测试。测试结果确认了三类真实 bug，本任务对其逐一修复。

## 修复的 Bug

### Bug 1：shm_seqlock 计数器数据竞争

**文件：** `src/shm_seqlock/ShmSeqlockComponent.cpp`  
**提交：** `bc21d29`

`OneShmSeqlockSender` 的三个统计计数器在并发发布时存在数据竞争：

| 字段 | 原代码 | 问题 | 修复 |
|---|---|---|---|
| `rejected_` (`atomic<uint64_t>`) | `load()+1; store()` | 两线程同时 reject 会丢一次计数 | `fetch_add(1, relaxed)` |
| `emptyWakes_` (`uint32_t`) | 在 `writing_.clear()` 之后（锁外）读写 | 非原子变量并发访问，数据竞争 | 改为 `atomic<uint32_t>`，用 `store`/`fetch_add` |
| `waiterResets_` (`atomic<uint64_t>`) | `load+store` 在锁外自增 | 同 `rejected_` 的丢计数问题 | `fetch_add` |

**严重性：** 中（计数器不准确，不影响数据正确性）

---

### Bug 2：shared_memory_broadcast publisher 侧 SIGSEGV

**文件：** `src/shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp`  
**提交：** `bc21d29`

`OneSharedMemoryBroadcastSender::publish()` 遍历 `clientListHead_` 链表时无跨进程锁，而 subscriber 析构函数（`~OneSharedMemoryBroadcastSubscription`）在另一个线程/进程中并发调用 `destroy_ptr` 释放链表节点，导致 publisher 解引用到已释放的共享内存对象，发生 SIGSEGV（use-after-free）。

**修复：** 在 `SharedMemoryBroadcastClientListItem`（共享内存头节点）中新增 `boost::interprocess::interprocess_mutex listMutex`：

- `publish()` 在遍历链表期间持有 `listMutex`
- `~OneSharedMemoryBroadcastSubscription()` 在写 `latestHeartbeat=0` 和任何 `destroy_ptr` 之前先获取 `listMutex`

**兼容性说明：** 此修改改变了 `SharedMemoryBroadcastClientListItem` 结构体的布局，与旧版本二进制不兼容。已有的 `/dev/shm/<segment>` 需在升级后删除重建。

**严重性：** 高（必现/偶现 SIGSEGV）

---

### Bug 3：aeron_ipc untethered 读端 term 重用导致数据损坏

**文件：** `src/aeron_ipc/AeronIpcComponent.cpp`、`tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp`  
**提交：** `05962f6`

**问题：** Aeron 零拷贝机制将共享 term buffer 的指针直接传给 fragment handler。当 untethered 读端的 handler 回调执行时间超过 `window-limit + linger` 超时之和时，driver 将该读端移出活跃集合，publisher 随后清零并复用该 term buffer，导致 handler 内的数据被覆盖。现有的 `malformed` 计数器无法检测此类损坏。

**触发条件（均需满足）：**
1. `tether=false`（默认）
2. 同一 stream 上有至少一个更快的读端（不同组件/Env）
3. 慢读端的 fragment handler（含 hook 回调）执行时间 > `window-limit + linger` 之和

**根因：** tm_kit 的默认超时（各 10ms）远短于 Aeron 自身默认值（各 5s），总保护时间仅 20ms，极容易被正常回调超过。

**修复（双重防御）：**

**Layer 1 - 调大默认超时：**

| 参数 | 旧默认值 | 新默认值 |
|---|---|---|
| `untetheredWindowLimitTimeoutMs` | 10ms | 2000ms |
| `untetheredLingerTimeoutMs` | 10ms | 2000ms |
| `untetheredRestingTimeoutMs` | 100ms | 100ms（不变）|

总保护时间从 20ms 提升到 4000ms，接近 Aeron 自身 5s 默认值。仍可通过 locator 属性覆盖。

**Layer 2 - 拷贝并校验帧头：**

`handleFragment()` 新逻辑：
1. 快照帧头（`aeron_header_values()`，记为 `before`）
2. 将 payload `memcpy` 到 polling 线程私有的 `fragScratch_` 缓冲区
3. 再次读取帧头（记为 `after`），与 `before` 比较
4. 不一致（term 被清零或轮转）则丢弃此帧，计入新增的 `overrun` 统计
5. 所有 client 回调均从本地拷贝分发，不再直接读 term buffer

**新增统计字段：** `AeronIpcSubscriberStats::overrun`

**严重性：** 高（静默数据损坏，`malformed` 计数器无法检测）

**参考：** Aeron GitHub [issue #1675](https://github.com/aeron-io/aeron/issues/1675)，[Flow and Congestion Control wiki](https://github.com/aeron-io/aeron/wiki/Flow-and-Congestion-Control)

## 测试验证

### 并发测试（ipc_concurrency_test）

```
SUMMARY pass=19 fail=0 risk=17 safe=3
```

相比修复前（`pass=18`），shared_memory_broadcast 测试 1 从 `probe` 升级为 `expect_no_crash`，测试 7 取消了单 reader comp 规避。

### aeron_ipc overrun 回归测试（aeron_ipc_overrun_test）

| 场景 | 修复前 badContent | 修复后 badContent |
|---|---|---|
| untethered + 快读端，stallMs=100ms，3 轮 | 5 / 2 / 5 | 0 / 0 / 0 |
| tethered + 快读端（对照）| 0 / 0 | 0 / 0 |

### 性能影响（正常场景，无慢 reader）

256B payload，tsc clock，custom sink，20us interval：

| 版本 | readers | p50 中位数 | p99 中位数 |
|---|---|---|---|
| baseline | 1 | 442 ns | 878 ns |
| fixed | 1 | 502 ns | 902 ns |
| baseline | 2 | 503 ns | 917 ns |
| fixed | 2 | 538 ns | 935 ns |

p50 涨约 7–14%（来自每条消息额外的 memcpy 和两次 `aeron_header_values` 调用），p99 涨约 2–3%（噪声范围内）。

## 测试程序变更

- `test/ipc_concurrency_test/run_tests.sh`：SMB 测试 1 改为 `expect_no_crash`，SMB 测试 7 取消单 reader comp 规避，新增 `expect_no_crash` 辅助函数
- `test/aeron_ipc_test/aeron_ipc_overrun_test.cpp`：新增 untethered overrun 复现测试

## 关联文档

- [IPC 并发测试任务文档](../20261103-ipc-concurrency-test/README.md)
