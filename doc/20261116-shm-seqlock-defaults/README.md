# ShmSeqlockComponent 默认参数调整

**任务开始时间：** 2026-11-16
**任务结束时间：** 2026-11-16
**状态：** 完成
**分支：** `feature/shm-seqlock-defaults`（commit `cc5c5b7`）

## 背景

基于 [ShmSeqlockComponent 系统测试报告](../20261113-shm-seqlock-system-test/REPORT.md) 的完整测试结果，调整 `ShmSeqlockComponent` 的默认 locator 参数，使默认选项更安全、更快，并保证使用者在不细读说明的情况下也不会用错。

## 目标与调整项

| 参数 | 原默认 | 新默认 | 依据 |
|---|---|---|---|
| `layout` | inline | inline-premark | 报告 §5.3：默认 `read=classic` 下端到端 p50/p99 由 462/902ns 降到 404/584ns（尾延迟约 −35%）；三种布局功能等价（778/778 全过） |
| `slotSize` | 1000 | 2048 | 用户拍板；覆盖更多中等消息，段内存约 8.3MiB（4096 槽） |
| `slotCount` | 4096 | 4096（不变） | 测试最充分的值；500k 条/s 下容忍窗口 8.19ms，快/慢 reader 均安全 |
| `writerLock` | false | true | 默认必须"不会用错"：多线程 publish（数据 + 心跳共段）默认串行化，消除 H04 风险（首次未加锁无法补救）；`writerLock=false` 显式写可强制关闭 |

## 方案（含被放弃的方案和原因）

- **slotSize**：曾考虑 4096（覆盖测试过的最大常规 payload）与 2048，用户拍板 2048——吞吐维度上 slotSize 不是杠杆（它管"最大消息尺寸"，不构成吞吐上限），变大只增加段内存与启动 prefault/memset 成本。
- **slotCount**：曾考虑 8192（500k 条/s 下 16.4ms 停摆容忍），确认 4096 已给 8.19ms 容忍、且是测试最充分的值，故维持不变。
- **layout**：inline 与 split 均可，选 inline-premark（默认 read=classic 下延迟最优）；split 的"被轮询行每条消息只写一次"更保守但实测稍慢。
- **writerLock**：直接改默认 true，`writerLock=false` 显式写即可强制关闭（`queryBool` 只在该字段缺省时才用默认值，机制不变，无需新语法）。

## 代码改动（按文件列出）

- `src/shm_seqlock/ShmSeqlockComponent.cpp`
  - `:274` `writerLock` 默认 `false → true`
  - `:279` `slotSize` 默认 `1000 → 2048`
  - `:283` `layout` 默认 `"inline" → "inline-premark"`
- `tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp`：同步更新头文件注释中的默认值说明（slotSize / layout / writerLock）。
- `test/shm_seqlock_test/ShmSeqlockTest.cpp`：新增 `defaults` 模式——裸 locator 验证三个新默认值（1500B 载荷接受/2500B 拒绝 = slotSize 2048；双线程并发 publish 无损坏 = writerLock=true；读段头断言 layoutVersion=3/slotCapacity=2048/slotCount=4096 = inline-premark）。
- `test/ipc_concurrency_test/run_tests.sh`：新增 `unlockprop()`；`#3/#4/#5/#6/#8` 无锁探针显式传 `writerLock=false`，保住"未加锁并发写"风险探针的意图（默认改为 true 后原本会静默变成已加锁）。
- 同步更新 `tm_kit_reference`（另仓库，未提交）：`tm_transport/03-transports/shm_seqlock.md`、`08-concurrency.md`、`05-heartbeat-and-alert.md`。

## 测试与验证（命令、构建类型、结果）

构建：`builddir_debug`（debug，gcc-toolset-15，`-j4`），仅编译 `shm_seqlock_test` 与 `ipc_concurrency_test` 目标。

- `shm_seqlock_test defaults` → **PASS**：received=2001 lost=0 bad=0 topicBad=0 rejected=1 layout=inline-premark slotSize=2048 slotCount=4096。
- `shm_seqlock_test stress --layout=inline|split|inline-premark --count=300000` → 三个 layout **全部 PASS**（bad/ooo/topicBad/corrupt 全 0）。
- `ipc_concurrency_test`（shm_seqlock）→ **pass=6 fail=0 risk=5 safe=0**。5 个"无锁"探针全部 RISK（corrupt>0）：显式 `writerLock=false` 确实关掉了锁（验证"强制关锁"生效）；`#5`（首个 locator 未加锁、后续 locator 加锁）也 RISK，复现 H04 风险；`writerLock=true` 的各场景（含心跳共段）全部 PASS。其中 `#5` 探针在首轮因 `writerLock=false,writerLock=true` 重复属性导致解析失败，已修复为替换式构造（`ALT=${CH/$UP/$LP}`），复跑通过。

## 兼容性与注意事项

- 默认 `layout` 改为 inline-premark 后，新建段带一个"始终预标记"的 slot，读者最多落后 `slotCount-1` 条（4096 槽下无感）；旧段/旧读者按段头布局走，无 ABI 影响。
- 默认 `writerLock=true` 只对"未显式写 writerLock 的 locator"生效；显式 `writerLock=false` 仍是合法的无锁快速路径（代价：调用方保证单线程 publish）。
- 默认 `slotSize=2048` 使默认段内存约 8.3MiB（原约 4.0MiB）。

## 相关文档（相关任务、tm_kit_reference 章节）
- 测试报告：[../20261113-shm-seqlock-system-test/REPORT.md](../20261113-shm-seqlock-system-test/REPORT.md)
- 参考文档：`tm_kit_reference/tm_transport/03-transports/shm_seqlock.md`、`08-concurrency.md`、`05-heartbeat-and-alert.md`
