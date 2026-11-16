# ShmSeqlock 系统测试统一入口

任务与完整规格：[开发记录](../../doc/20261113-shm-seqlock-system-test/README.md)、[PLAN](../../doc/20261113-shm-seqlock-system-test/PLAN.md)。

本目录负责进程控制、原始字节和边界 oracle、确定性崩溃/futex 屏障、无注入黑盒及 leader 汇总。图、真实 Heartbeat、资源/性能各为独立程序；`suite/` 将这些程序和现有基线集中构建，避免全项目配置中无关网络依赖展开。

当前状态（2026-11-13）：核心功能矩阵（含 F10/F11/R03/R05/R06/R07/R08/R-dirty-ext/R-risk/R-resource）release/debug/ASan 三个构建均 **778/778 PASS**，夜间 `--repeat=20` **15,560/15,560 PASS**；图 192/192、heartbeat、baseline 通过；resource 因已知缺陷 DEF-1（callback 自移除后段映射保留到组件析构）保留 1 个 FAIL，套件整体退出码 1 属预期。DEF-1 已决定不修复，作为已知限制保留（机会式回收实验已回滚，未进入生产代码）；1 个 FAIL 与退出码 1 属长期预期。缺陷、证据与待办见[任务记录](../../doc/20261113-shm-seqlock-system-test/README.md)。

```sh
source /data/hxli/codes/tm_kit/build-common/build-env-setting.sh
meson setup builddir_suite_release test/shm_seqlock_system_test/suite --buildtype=release
ninja -j4 -C builddir_suite_release
python3 test/shm_seqlock_system_test/run_suite.py builddir_suite_release --matrix --baseline --out=/tmp/shm-suite-unique
```

输出目录须不存在；汇总退出码非零表示存在失败，不会豁免已知资源缺陷。`--only core|graph|heartbeat|resource|baseline` 可重复选择。基线含三种 layout 的现有脚本与 shm_seqlock 并发脚本。各子目录保留自身运行入口及覆盖说明。

只编译控制/故障测试时：

```sh
meson setup builddir_system_debug test/shm_seqlock_system_test/standalone \
  --buildtype=debug -Dtm_include="$INSTALL_PREFIX/include" \
  -Dboost_include="$VCPKG_ROOT/installed/x64-linux/include"
ninja -j4 -C builddir_system_debug
python3 test/shm_seqlock_system_test/run_tests.py \
  --binary builddir_system_debug/shm_seqlock_system_worker \
  --matrix --seed=20261113 --repeat=1 --log-dir=/tmp/shm-core-unique
```

`--case` 选择单类，`--layout`、`--read`、`--writer-wait`、`--reader-wait` 控制参数；`--event` 可指定故障窗口，`--timeout` 控制单例 watchdog（默认30秒）。`--matrix` 生成全部 24 组核心参数、适用的精确窗口、futex 交错和所有有向布局转换，再按固定 seed 打乱顺序。夜间可设 `--repeat=20`；重复次数与长跑实际结果分开报告。

| 用例 | 验证内容 |
|---|---|
| C00 | 确定性错误topic/payload、逆序、重复必须被oracle检出 |
| F01-F02 | reader先启动/后启动、多进程reader、空和二进制topic/payload、缓存行/320B预测边界与恰满 |
| F03 | 反复超一拒绝后合法发布，publisher rejected和reader received精确对账 |
| F04-F05 | 精确/regex过滤、hook往返/nullopt、扩展后超限；过滤前received语义 |
| F07 | latest/next、静止活writer、死writer残留与接管 |
| F08 | 首次sender属性、读端忽略layout属性、同进程不同component锁竞争 |
| F09 | 名字/数值/layout非法，失败后同组件可重试；尾随字符和未知属性按当前宽松解析记录 |
| F10 | 4个查询线程并发调用 reader/publisher stats 与 threadHandles，同时100次 add/publish/remove；结束时无残留 reader |
| F11 | CPU lease provider：缺失时明确报错、成功绑定传播 who/spec、重复安装抛异常、provider 抛异常后可回退创建 |
| R01 | callback屏障暂停，精确推进N−1/N/N+1/2N+1，按layout对账套圈和lost |
| R03 | 同段慢/快 callback 共享 reader 线程：慢 callback 阻塞时快 callback 不执行，释放后全部交付 |
| R04-crash | 独立pipe确认持锁/创建/magic、odd/半写metadata/topic/payload、even/published/premark、superseded、恢复扫描/epoch窗口后SIGKILL并接管 |
| R04-transition | 更换layout/count/capacity/stride，跨全部有向layout转换追随新段 |
| R-futex | 登记前后/睡眠前后与publish交错，继续收发与停止 |
| R-waiter-leak | kill已登记reader，连续空唤醒清理泄漏waiter，新reader继续 |
| R-writer-conflict | 8竞争者明确失败、SIGSTOP仍持锁、SIGKILL后可接管 |
| R-blackbox | 无注入worker按固定seed在发送时随机SIGKILL，精确检查已完成区间和可选最后一条，再接管 |
| R-dirty | 零长度/短头/零magic/无效头，reader先启动，新writer重建后正常 |
| R-dirty-ext | 结构合法但字段异常的头：valid/odd seq/超长长度/错误topicHash/published越过环；reader不崩溃且可退出，之后新段恢复正常 |
| R05 | 4线程同段并发 add，再同时 remove（含重复/未知 id），最后订阅停止与并发移除竞态；结束后无残留 reader |
| R06 | callback 移除其他订阅；callback 自移除并从 reader 线程重新订阅；旧订阅停止、新订阅继续交付 |
| R07 | 最后订阅退出后以不同 layout/read/wait 重新添加；重复/未知 remove 安全 |
| R08 | 进程重连 churn：默认200次（`SHM_R08_CYCLES=1000` 可跑满1000次）reader start/receive/quit，每50次 SIGKILL writer 并接管 |
| R-risk | 风险探测（不承诺安全）：callback 抛异常、publisher 捕获对象超出组件生命周期、活跃映射 truncate、替换锁文件后第二 writer |

每个case登记唯一段名和全部子进程，清理先kill/reap再unlink登记的段和锁文件。每例结果增量追加到 `results.jsonl`，运行结束再写完整 `results.json`；夜间 `--repeat=20` 因此不会产生 O(n²) 的全量重写。stdout含控制命令与单调时间，stderr独立保存。就绪分别记录API返回和映射起点；普通功能测试通过逐条交付建立受控无丢失区间，不用这些轮次宣称性能。每阶段默认5秒；超时保存线程栈（权限不足保留错误）、状态、wchan、maps和子进程退出码。数据包含任意NUL和空字节，oracle比较整个topic/payload；按原始字节登记的空/小报文不强加测试头。

`TM_KIT_SHM_SEQLOCK_TEST_PROBES` 只传给 instrumented component/worker target。内部 `TestProbe.hpp` 不安装，不增加公共API，不改共享内存布局；未定义宏时事件消除，分段拷贝宏恢复单次原始 `std::memcpy`。生产黑盒、图、Heartbeat、性能与现有基线链接无注入component。可用 `nm -C libproduction_shm_seqlock.a` 确认无 `systemTestProbe`/`systemTestCopy` 符号。

ASan/UBSan 使用独立 debug 目录和 `-Db_sanitize=address,undefined`。当前执行环境 LeakSanitizer 报 ptrace fatal error，实际使用 `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`；这不代表泄漏检查通过。R-resource 的 `RLIMIT_NOFILE=5` 探测在 sanitizer 下 SKIP（ASan runtime 在 main 前需要描述符），该失败路径由 release/debug 覆盖。TSan不能证明跨进程共享内存无撕裂。

本轮尚未完成的全量规格（`/dev/shm` 满/mmap 失败/权限组合、序号极值、所有参数边界、2h/24h 混合长跑、正式五轮性能矩阵等）必须以开发记录中的覆盖表为准。入口存在不等于已运行，任何风险未观察到也不等于安全。
