# ShmSeqlock 图系统测试

独立 Linux C++17 测试程序。覆盖 raw/typed 图、MultiTransport topic、默认/显式 typed hooks、无效 CBOR/错误类型/截断、四线程 publisher、图与直接 publisher 共段、importer stop/restart、raw/typed fetch、动态 listener 请求/响应与 SynchronousRunner 管理路径，以及同步/异步慢下游。

从本 worktree 根目录构建（依赖已安装的 tm_infra/tm_basic，直接编译本 worktree 的 shm_seqlock 实现）：

```sh
source ../../build-common/build-env-setting.sh
meson setup builddir_graph_debug test/shm_seqlock_graph_system_test/standalone \
  --buildtype=debug --prefix="$INSTALL_PREFIX" \
  --cmake-prefix-path="$VCPKG_ROOT/installed/x64-linux"
ninja -j1 -C builddir_graph_debug shm_seqlock_graph_system_test
python3 test/shm_seqlock_graph_system_test/run_tests.py \
  builddir_graph_debug/shm_seqlock_graph_system_test --matrix \
  --output /tmp/shm-graph-debug-matrix
```

全项目 Meson 集成入口为本目录 `meson.build`，由 `test/meson.build` 添加 subdir 即可。`standalone` 避免探测与这些测试无关的全部 transport 依赖，不修改系统或 vcpkg。

驱动支持 `--case`（可重复）、`--layout`、`--read`、`--wait`、`--repeat`、`--seed`、`--timeout`、`--output`。默认每场景 30 秒外部 watchdog；`--matrix` 跑 3 layout × 2 read × 2 同 wait 组合。混合 writer/reader wait 由主系统测试控制面覆盖，此图驱动不声称覆盖 24 组。

每例在独立进程、唯一段名上运行。启动使用收到的哨兵确认映射与图下游可用；正式报文不重试。内容 oracle 校验完整 topic/payload、精确数量、重复、单 writer 顺序和各线程内顺序。`concurrent`/`shared_exporter`/`threaded_exporter` 先登记全部消息，4 个线程各发 250 条，小于 8192 槽环容量，验收不允许丢包。测试中的 `writerLock=true` 在首次创建 sender 时生效。

`slow_sync`/`slow_async` 用条件变量暂停第一条业务消息的 sink，在暂停期间发 999 条，分别验证 reader 停顿/继续读取；释放后要求全部 1000 条正确交付、transport lost=0。此为积压层级验证，未执行超过环容量的套圈压力。

结果在 `results.json` 中记录 case、完整命令、seed、参数、重复轮次、退出码、超时和日志路径。超时保存 `/proc` 状态及每线程 stack/wchan（权限不足按错误原样记录），结束子进程后只清理该例段和锁文件。程序打印 PASS 之后异常退出仍判 FAIL。

`control` 每次停止前先确认哨兵为最新值，避免 latest 合法重放被当作上一订阅区间的重复；验证 20 次完成可观察 stop 后 restart，异步控制接口没有完成 future，因此不声称覆盖所有交错重复命令。正式吞吐、延迟、长跑和 sanitizer 结果由主任务分别记录。

任务记录：[GRAPH.md](../../doc/20261113-shm-seqlock-system-test/GRAPH.md)。
