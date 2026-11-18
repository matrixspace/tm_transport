# 数据库轮询导入器停止评审修复

**任务开始时间：** 2026-11-18  
**任务结束时间：** 2026-11-18  
**状态：** 完成  
**分支：** `fix/20261118-review-transport`（代码提交 `cfd4c8f`）

infra 评审指出 UniformSimpleImporter 等待已进入的回调时，会卡在重复数据库导入器的轮询休眠。两种重复导入器将使用 infra 的停止上下文，唤醒轮询等待并在读取行之间检查停止请求。首轮标记和已见键按导入器实例保存。

此处可取消轮询等待，正在执行的 SOCI 查询或驱动读取仍受后端超时和返回行为约束。

## 代码和验证

- `DBTableImporterFactory.hpp` 的普通和按键去重重复导入器均在首轮后使用 `ImporterStopContext::waitForStop`，并在处理结果行前检查停止。
- `db_poll_stop_test` 用 SQLite 内存表验证两种导入器都能收到首批数据、析构唤醒一小时轮询等待，独立实例不共享首轮标记和键集合。
- GCC 15，Meson debug，运行 `ninja -j2 -C builddir_debug test/db_poll_stop_test/db_poll_stop_test` 与 `meson test -C builddir_debug --no-rebuild --print-errorlogs --repeat 5 db_poll_stop_test`，5/5 通过。
- 现有 `threaded_handler_transport_test` 无法打开 39871 监听端口。独立回环 socket 测试返回 `Operation not permitted`。gRPC 路径未做运行验证。

相关记录：[评审报告](../../../review.md)、[infra 停止上下文](../../../tm_infra/doc/20261118-realtime-queue-deque-optimization/review-fixes.md)、[basic 停止线程](../../../tm_basic/doc/20261118-graceful-thread-stop/review-fixes.md)。
