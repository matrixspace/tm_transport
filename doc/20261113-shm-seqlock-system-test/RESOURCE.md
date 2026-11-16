# 性能、资源与黑盒长跑子任务

**开始：** 2026-11-13
**状态：** 子任务完成。leader 统一验收复现已知资源失败（DEF-1：18 PASS / 1 FAIL），保留严格断言。机会式回收修复实验验证后已按用户要求回滚；用户最终决定 **DEF-1 不修复**，作为已知限制。实验数据见 [DEF1_FIX_EXPERIMENT.json](DEF1_FIX_EXPERIMENT.json)。
**分支：** `feature/shm-system-perf`（基线 `b6ff6e7`，代码 commit `a0262ec`，清理健壮性补充 `5cc26bb`）

**实现结束：** 2026-11-13

本记录属于[系统测试任务](README.md)；总方案见 [PLAN.md](PLAN.md)。leader 统一构建与执行验收。

## 方案

- 独立测试 executable 直接编译未经注入的组件源文件，不依赖主 library 全量构建。
- 性能轮次仅校验头、topic 和序号，使用有界直方图统计延迟；另提供内容逐字节校验轮次。按预定单调时钟开放式发送，不用 reader ACK 控制测量速率。
- 资源轮次分别对外部移除与 callback 自移除，采样同一个仍存活组件的 RSS/PSS、FD、映射和线程；单独采样析构后，保留增长证据。
- Python 入口保存每轮原始输出、系统环境、JSON 汇总和 watchdog 诊断，提供 smoke/full/soak/churn 模式。2h/24h 长跑仅提供入口，本子任务不启动。

## 进度

- 已读任务 README/PLAN、工作区 CLAUDE.md、reference 索引与 shm_seqlock 全章。核对 `stoppedSubscriptions_` 自移除路径；将积累与析构释放分开报告。
- 确定 standalone Meson 配置：全仓库 Meson 在无关 grpc/libetcdcpp 的 pkg-config 探测耗时，已中断；独立 project 仅用工作区环境提供的头文件、threads 和 rt，不修改 `.pc` 或第三方。release 窄目标 4 步编译链接完成（GCC 15.2.1），警告来自现有 ByteData 头文件。

## 构建入口

```bash
source /data/hxli/codes/tm_kit/build-common/build-env-setting.sh
meson setup builddir_resource_release test/shm_seqlock_resource_system_test/standalone --buildtype=release
ninja -j1 -C builddir_resource_release
python3 test/shm_seqlock_resource_system_test/run_tests.py builddir_resource_release/shm_seqlock_resource_system_test --profile=smoke --out=/tmp/shm-resource-smoke
```

独立 project 根据 `INSTALL_PREFIX` 与 `VCPKG_ROOT` 取 include 根，不保存个人绝对路径。常规仓库构建时由 leader 在 `test/meson.build` 引入本目录即可。

## 已执行验证与发现

- 构建：`source .../build-common/build-env-setting.sh; meson setup builddir_resource_release test/shm_seqlock_resource_system_test/standalone --buildtype=release; ninja -j1 -C builddir_resource_release` 成功。日志 `/tmp/shm-resource-standalone-meson.log`、`/tmp/shm-resource-standalone-build.log`，后续增量日志 `/tmp/shm-resource-standalone-rebuild.log`。
- `python3 -m py_compile test/shm_seqlock_resource_system_test/run_tests.py` 成功。
- `--mode=oracle`：128 个确定性逐字节损坏全部检出；直方图量化范围检查通过。
- `--mode=perf --readers=1 --rate=1000 --warmupMs=20 --durationMs=100 --wait=futex --verify=true` 100/100 正确接收，无丢失/损坏/逆序。
- 短 smoke（12 组 layout/read/wait 内容校验、oracle、普通/自移除 20 轮）汇总 14 PASS / 1 FAIL。原始与 JSON 汇总在 `/tmp/shm-resource-agent-smoke/`。该轮只验证基础实现，不能当作正式性能基线。其后 smoke 入口扩展了 0/2/4/8 reader 快速轮次，待 leader 统一执行。
- **确定性资源积累：** `--mode=churn --cycles=5 --wait=futex --selfRemove=true` 组件仍存活时段映射数由 1（writer）增长至 6；各轮线程已退出。reader 组件析构后映射回到 1，FD 回基线。20 轮 smoke 同样输出 FAIL。该现象与源码 `stoppedSubscriptions_` 延后析构吻合；未修改生产组件，未放宽资源断言。
- 修正测试自身计时：末条按计划发送完成后仍计入最后一个发送间隔，吞吐/CPU 分母覆盖完整测量时长。

## 当前交付边界

实现只涉及独立测试与本记录，不改变生产 API、ABI 或行为。正式 5×30s 性能矩阵与 2h/24h soak 入口齐备但本子任务未执行，由 leader 汇总安排。10k churn 与 spin/futex 单轮 5s+30s 测量已按 leader 指示追加执行，结果见下文。未执行的长跑与性能基线不得记作通过。功能图与 Heartbeat 的性能维度由其他程序/leader 组合；本程序覆盖直接组件路径。
- 额外边界轮次：`--mode=perf --readers=0 --rate=0 --warmupMs=5 --durationMs=30 --wait=futex` PASS（75,994 offered）；8-reader 饱和 100ms PASS（162,254 offered，各 reader 有可核算丢失，无损坏/顺序错误）。原始 JSON `/tmp/shm-resource-zero-reader.json`、`/tmp/shm-resource-eight-reader.json`。吞吐数字仅是短功能轮次证据，未作为基线。
- 最终清理审查：默认段名与 Python 登记段名增加单调时间戳；C++ 仅在成功取得 publisher 后标记清理所有权。release 增量重编译、oracle 再通过；同一显式段名的双进程冲突验证 PASS：失败者退出 1，活 writer 的段 inode/锁文件保留，原 owner 正常退出后才清理。证据 `/tmp/shm-resource-cleanup-conflict.json`；日志 `/tmp/shm-resource-cleanup-rebuild.log`。

## Leader 追加执行：10k churn 与单轮 release 测量

2026-11-13 在同一独立 worktree，使用清理健壮性补充后的 release 二进制执行；没有修改生产代码。

### 10k 普通/自移除

```bash
python3 test/shm_seqlock_resource_system_test/run_tests.py builddir_resource_release/shm_seqlock_resource_system_test --profile=churn --cycles=10000 --out=/tmp/shm-resource-agent-churn-10k
```

Oracle PASS；普通 churn PASS；自移除 churn FAIL，汇总退出 1。原始 stdout/stderr、每轮 JSON、环境和汇总保存于 `/tmp/shm-resource-agent-churn-10k/`，入口控制台 `/tmp/shm-resource-agent-churn-10k.log`。

| 指标 | 普通：基线 → 组件存活 10k → 析构后 | 自移除：基线 → 组件存活 10k → 析构后 |
|---|---|---|
| 段映射数（含 writer） | 1 → 1 → 1 | 1 → 10,001 → 1 |
| 总映射数 | 42 → 46 → 46 | 42 → 30,044 → 52 |
| FD | 5 → 5 → 5 | 5 → 5 → 5 |
| 线程 | 1 → 1 → 1 | 1 → 1 → 1 |
| RSS KiB | 2,048 → 4,012 → 4,012 | 2,124 → 159,628 → 39,684 |
| PSS KiB | 1,115 → 1,189 → 1,189 | 1,120 → 116,841 → 36,790 |
| callback 数 | 10,000 | 10,000 |

普通 churn 无段映射积累；自移除精确积累 10,000 个 reader 映射，并保留已结束线程的待 join 对象。析构确实释放这些映射，allocator/线程缓存使 RSS 没有完全回到最初值，不能混淆为仍有共享段映射。该失败是已有实现行为，断言保持严格。单轮 wall time 约 1.27s / 1.72s，只用于执行证据，不作为 churn 延迟 SLA。

### 单轮 release spin / futex

```bash
python3 test/shm_seqlock_resource_system_test/run_tests.py builddir_resource_release/shm_seqlock_resource_system_test --profile=full --repeat=1 --readers=1 --payloads=256 --layouts=inline --reads=classic --waits=spin,futex --rate=10000 --out=/tmp/shm-resource-agent-perf-1round
```

运行配置为未经注入的 release、每项 5 秒预热 + 30 秒测量、1 reader、inline/classic、256B payload、2B topic、slotCount=4096、mlock=false、writerLock=false、默认 spinCount=20000，seed=20261113 随机顺序为 futex 后 spin。Oracle 与两项测量全部 PASS，入口退出 0；原始输出和环境在 `/tmp/shm-resource-agent-perf-1round/`，控制台 `/tmp/shm-resource-agent-perf-1round.log`。版本化摘要与原始汇总 SHA256 见 [RESOURCE_RESULTS.json](RESOURCE_RESULTS.json)。

| 指标 | spin | futex |
|---|---:|---:|
| offered / accepted / received | 300,000 / 300,000 / 300,000 | 300,000 / 300,000 / 300,000 |
| 实际 offered / received 每秒 | 10,000 / 10,000 | 10,000 / 10,000 |
| lost / rejected / bad / order | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| publish 平均 ns | 175.521 | 2,526.24 |
| publish p99 ns | 340 | 3,232 |
| 发送前 → callback p50 / p90 / p99 ns | 584 / 768 / 896 | 5,184 / 5,952 / 6,464 |
| 发送前 → callback max ns | 10,123 | 136,731 |
| 计划发送时刻 → callback p99 / max ns | 1,136 / 328,026 | 6,720 / 6,470,143 |
| 整进程 CPU（单核 100%，含 writer） | 199.369% | 169.391% |
| 测量期间 RSS KiB | 6,116 → 6,116 | 6,012 → 6,316 |
| FD / 线程 / 总映射 / 段映射 | 5 / 2 / 47 / 2，均稳定 | 5 / 2 / 47 / 2，均稳定 |

测试机为 Intel Xeon Gold 5117 @ 2.00GHz，Linux 4.18.0-372.9.1.el8.x86_64，56 个在线逻辑 CPU、2 个 NUMA 节点；GCC 15.2.1。进程未绑核，亲和集合 0–55；频率策略/当前频率 sysfs 项不存在，已在环境中明确记录 unavailable。二进制及源版本哈希随 JSON 保存。

## Leader 统一验收（2026-11-13 当前 HEAD）

`run_suite.py builddir_suite_release --matrix --baseline` 中 resource smoke 为 **18 PASS / 1 FAIL**，唯一失败仍是第 18 轮 `churn cycles=20 selfRemove=true`（`retained_mappings_while_alive=true`），与上述 10k 结论一致；oracle、12 组 layout/read/wait 内容校验、0/2/4/8 reader 与普通 churn 全部通过。证据 `/tmp/shm-system-leader-final2/resource`。缺陷详情与候选修复方向见[主记录 README 的“已知缺陷 DEF-1”](README.md)。

**2026-11-14 全量性能矩阵：** 1200 个配置、5 轮×30s，零丢失/损坏/乱序，数据见 [PERF_FULL_RESULTS.json](PERF_FULL_RESULTS.json)。**2026-11-14 72h 多路稳定性**已启动，见[主记录稳定性章节](README.md)。

**2026-11-13 staged 性能基线与负载测量：** 5 轮×30s、pin NUMA0 物理核；26 个 10k/s 配置全部零丢失；饱和吞吐 0 reader 约 4.5M/s、8 readers 约 1.6–1.7M/s；50%/90% 负载 8 个配置全部零丢失。慢 reader writer 成本和 futex 空闲 CPU 达标。版本化结果见 [PERF_BASELINE.json](PERF_BASELINE.json)、[LOAD_RESULTS.json](LOAD_RESULTS.json)，详见[主记录的性能测试章节](README.md)。

**DEF-1 修复实验（已回滚，最终不修复）：** 机会式回收方案曾使 resource smoke 达到 19/19、10k 自移除段映射保持 1，但用户评估认为其对其它代码路径影响较大，要求回滚；生产代码已恢复基线，重新构建后 smoke 回到 18/19。用户随后决定 DEF-1 不修复，作为已知限制保留。实验前后对照与原始证据见 [DEF1_FIX_EXPERIMENT.json](DEF1_FIX_EXPERIMENT.json)，不作为当前交付结果。

**限制：** 每配置只有 1 轮，未达到方案规定的至少 5 轮，也未做同机历史双批对照；不能据此判性能回归或固化正式基线。rate=10000 为选定负载，未经容量校准，不能声称是 50%/90% 负载。整机未独占，其他 agent/任务可能同时运行。默认 10k/s 和 spinCount=20000 使 futex 轮次仍频繁自旋，此 CPU 指标包含 writer，不能套用低频 reader 单核 <5% 门槛。每 reader 仅 300,000 样本，按方案省略 p99.9。性能轮次仅校验头/topic/顺序，内容逐字节由此前功能轮次验证。未执行 2h/24h 长跑或巨大 full 矩阵。
