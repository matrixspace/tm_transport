# aeron_ipc untethered 超时默认值改回 10ms

**任务开始时间：** 2026-11-13  
**任务结束时间：** 2026-11-13  
**状态：** 完成  
**分支：** `fix/aeron-untethered-defaults`（基于 `feature/ipc-thread-affinity` `3eed176`），commit `8bb9d5c`，已 fast-forward 合并回 `feature/ipc-thread-affinity`，临时分支已删除

## 背景

`aeron_ipc_test` 的 test3（一快一慢两个读者，慢读者每条睡 200µs，发布者 50kHz 发 50 万条，要求快读者
零 gap）自 `05962f6`（2026-11-04）起一直失败。该提交把 `untetheredWindowLimitTimeoutMs` /
`untetheredLingerTimeoutMs` 默认值从 10ms 改为 2000ms，导致慢读者每次能堵住发布者约 2s 才被移出；
`backPressure=drop` 下这段时间的消息全部被发布者丢弃，所有读者（包括快读者）都收不到。

实测（release，绑核分支二进制）：默认值下发布者背压 421631 次、快读者只收 ~7.8 万条；
只把 test3 locator 的两个超时改成 10ms，快读者 500000 条 0 gap、背压 0 次，慢读者 `malformed=0`。

头文件注释写的默认值仍是“10 / 10 / 100”，与代码不一致。

## 方案

用户选定方案 1：两个默认值改回 10ms。

`05962f6` 是两层防护：第一层是加长超时，第二层是 `handleFragment` 先把 fragment 拷到线程私有缓冲区、
再比对帧头，被复用的 term 计入 `overrun` 丢弃，不把损坏数据交给用户。第二层已足以保证正确性，
2s 默认值只是减少慢读者自身的丢包，代价由所有读者承担。改回 10ms 后慢读者自己丢消息（它本来跟不上），
其他读者不受影响；需要更长保护期的部署可在 locator 上调大。

被放弃的方案：保留 2s 只改测试（默认配置下慢读者拖累全体，与“慢读者不能拖住发布者”的承诺不符）；
折中 100ms（没有数据支撑选哪个值）。

## 代码改动

| 文件 | 改动 |
|---|---|
| `src/aeron_ipc/AeronIpcComponent.cpp` | `untetheredWindowLimitTimeoutMs` / `untetheredLingerTimeoutMs` 默认值 2000 → 10；注释改为说明短超时的取舍和 `handleFragment` 帧头复检兜底 |
| `tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp` | 类注释写明默认值 10 / 10 / 100、调大超时的代价（`backPressure=drop` 下所有读者丢消息），以及被移出的读者不会看到撕裂数据（计入 `overrun`） |

`05962f6` 的第二层防护（拷贝 + 帧头复检）保持不变。

## 测试与验证

构建：release（`builddir_release`，`ninja -j2`）。

| 验证项 | 命令 | 结果 |
|---|---|---|
| aeron 回归 ×3 | `test/aeron_ipc_test/run_tests.sh builddir_release` | 每次 8/8 PASS；test3 快读者 500000 条、0 gap、背压 0，慢读者 ~2.9 万条 |
| overrun（无快读者） | `aeron_ipc_overrun_test <dir> false 30/100/500 5 65536`、`true 100` | 4 组全部 `changed=0 badContent=0 malformed=0` |
| overrun（带快读者，`05962f6` 针对的 term 复用场景） | `aeron_ipc_overrun_test <dir> false/true 100/500 5 65536 true` | 4 组全部 `changed=0 badContent=0 malformed=0 fastBad=0`；tether=false 时快读者收到 2902950 / 13400167 条（≈全部发布量），tether=true 时被慢读者拖到 698468 / 363239 条 |
| aeron 并发套件 | `test/ipc_concurrency_test/run_tests.sh ... aeron_ipc` | pass=4 fail=0；RISK 项为脚本标注的无锁多写不支持用法，与改动前一致 |
| aeron 绑核用例 | `IPC_AFFINITY_CASE=aeron_ipc ipc_thread_affinity_test` | 38/38 |

## 兼容性与注意事项

- 行为变化（默认值，不改 API）：默认配置下慢读者 ~20ms 内被移出、休息 100ms 后重入，只丢它自己跟不上的消息；
  其他读者与发布者不再被周期性拖住。这恢复的是 `05962f6` 之前（2026-11-04 前）的默认行为。
- 需要更长保护期的部署（例如偶发长回调但宁可拖慢全体也不想慢读者丢包）在 locator 上显式写
  `untetheredWindowLimitTimeoutMs` / `untetheredLingerTimeoutMs`。
- 慢读者 handler 运行期间 term 被复用时，fragment 被丢弃并计入 `AeronIpcSubscriberStats::overrun`，不会交付撕裂数据。

## 相关文档

- 原修复：[IPC Transport 并发 Bug 修复](../20261103-ipc-concurrency-bugfix/README.md)（`05962f6`，两层防护）
- 发现场景：[IPC Transport 内部线程绑核](../20261112-ipc-thread-affinity/README.md) 回归中 aeron test3 失败
