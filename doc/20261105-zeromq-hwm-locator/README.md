# ZeroMQ 通过 ConnectionLocator 配置 HWM

**任务开始时间：** 2026-11-05  
**任务结束时间：** 2026-11-05  
**状态：** 完成  
**分支：** `develop`（commit `990cd78`，已推送）

## 背景

ZeroMQ PUB/SUB socket 的高水位标记（HWM）原来只能用 libzmq 默认值 1000。突发流量超过水位时 publisher 会直接丢弃消息，用户没有办法调整。本任务让 locator 的属性块可以配置 HWM。

## 用法

| 属性 | 作用对象 | 说明 |
|---|---|---|
| `hwm` | publisher 和 subscriber | 通用默认值 |
| `sndhwm` | publisher（`ZMQ_SNDHWM`） | 覆盖 `hwm` |
| `rcvhwm` | subscriber（`ZMQ_RCVHWM`） | 覆盖 `hwm` |

```
zeromq://localhost:12345[hwm=50000]
zeromq://ipc:0:::/tmp/feed[sndhwm=0,rcvhwm=20000]
```

- 值必须是非负整数，`0` 表示不限制；不设置时保持 libzmq 默认值 1000。
- 非法值（非数字、负数、超出 `int` 范围）抛出 `std::invalid_argument`，信息中包含属性名和 locator。
- socket 按 host/port/identifier 共享，HWM 只在 socket 首次创建时生效；之后指向同一 socket 的 locator 即使带了不同 HWM 也不会修改已有设置。
- 调大水位只能推迟丢弃，不能保证不丢。

## 代码改动

- `src/zeromq/ZeroMQComponent.cpp`：新增 `parseHwm`；PUB socket 在 `bind` 前设置 `sndhwm`，SUB socket 在 `connect` 前设置 `rcvhwm`。
- `tm_kit/transport/zeromq/ZeroMQComponent.hpp`：注释说明三个属性及共享 socket 的限制。
- `test/zeromq_test/ZeroMQTest.cpp`：
  - 用例 10：三组合法属性组合，每组突发 5000 条消息（超过默认水位 1000），验证全部收到。
  - 用例 11：`hwm=abc`、`hwm=-1`、`sndhwm=12x`、`rcvhwm=99999999999` 都抛出 `std::invalid_argument`。

## 验证

- debug、release 构建均成功，日志中无 `FAILED:` / `error:`。
- 两种构建下 `zeromq_test` 11 个用例全部通过。
- 已安装到开发环境 `install/`（头文件、`libtm_kit_transport.a`、`libtm_kit_transport_debug.a`）。
- 下游 tm_examples 用 `ASSUME_YES=1 ./build.sh ../tm_examples`（debug）清理重建：编译链接成功，`build_debug.log` 中无 `FAILED:` / `error:`。`build.sh` 最后的安装校验报错（找不到 `tm_kit_examples_debug.pc`），原因是 tm_examples 本来就没有安装目标（`Nothing to install.`），与本次改动无关。

## 相关文档

- `tm_kit_reference/tm_transport/03-transports/zeromq.md`：新增属性说明，并修正因行号下移而过期的源码引用（commit `e0dde0c`，已推送到 `main`）。
