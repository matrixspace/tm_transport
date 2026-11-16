# ShmSeqlockComponent 读端 join 默认值改为 next

**任务开始时间：** 2026-11-16
**任务结束时间：** 2026-11-16
**状态：** 完成
**分支：** `feature/shm-seqlock-join-default`（commit `b9e6442`）

## 背景

用户要求把读端参数 `join` 的默认值从 `latest` 改为 `next`，并接受连带影响（fetch-first、心跳发现都随默认变为 next）。

## 目标与调整项

| 项 | 原值 | 新值 | 依据 |
|---|---|---|---|
| `join` 默认 | `latest`（首条交付已发布的最新消息） | `next`（只交付加入之后发布的消息） | 用户拍板：默认读者只看加入之后的新消息，不重放历史最新一条 |

连带影响（用户确认接受，一并改）：
- `fetchFirstUpdateAndDisconnect` / `fetchTypedFirstUpdateAndDisconnect`：从"取最新一条"变为"等加入之后发布的第一条"。
- 心跳/服务发现：新订阅者要等下一个心跳周期才能发现服务（不再立即拿到最近一条心跳）。

## 方案（含被放弃的方案和原因）

- 曾考虑把 fetch-first 显式钉成 `join=latest` 以保留其"取最新"契约（方案 A），用户选择方案 B：全部随默认改为 next，并同步改 fetch-first 的报告定义。

## 代码改动（按文件列出）

- `src/shm_seqlock/ShmSeqlockComponent.cpp`：`joinLatest_(locator.query("join", "latest") != "next")` → `joinLatest_(locator.query("join", "next") == "latest")`（缺省即 next；显式 `latest` 仍可用）。
- `tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp`：头文件注释把 `join` 默认标记从 latest 移到 next。
- `test/shm_seqlock_test/ShmSeqlockTest.cpp`：`defaults` 模式新增 join=next 校验（先发布一条"pre"再订阅，断言 pre 不被交付）。

## 测试与验证（命令、构建类型、结果）

构建：suite standalone 项目 `test/shm_seqlock_system_test/suite`（debug，gcc-toolset-15，`-j4`），仅编译 `shm_seqlock_test`（ShmSeqlockComponent.cpp + ConnectionLocator.cpp + ShmSeqlockTest.cpp）。

- `shm_seqlock_test defaults` → **PASS**：`join=next`（预置消息不交付）、layout=inline-premark、slotSize=2048、slotCount=4096、writerLock=true（received=2001 lost=0 bad=0 rejected=1）。
- `shm_seqlock_test stress --layout=inline|split|inline-premark --count=300000` → 三个 layout **全部 PASS**（bad/ooo/topicBad/corrupt 全 0）。

## 兼容性与注意事项

- 不向前兼容：依赖"读者加入时立即收到最近一条"的上层代码（fetch-first、心跳发现）行为改变。显式 `join=latest` 可恢复旧行为。

## 相关文档（相关任务、tm_kit_reference 章节）
- 测试报告：[../20261113-shm-seqlock-system-test/REPORT.md](../20261113-shm-seqlock-system-test/REPORT.md)
- 参考文档：`tm_kit_reference/tm_transport/03-transports/shm_seqlock.md`、`01-broadcast.md`
