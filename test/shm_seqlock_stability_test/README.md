# ShmSeqlock 多路长时间稳定性测试

任务记录：[README](../../doc/20261113-shm-seqlock-system-test/README.md)。

本目录包含一个自驱动的 stability worker 和一个多路编排器。worker 直接链接生产组件（无故障注入），writer 按指定速率发送带 generation/序号/大小/校验和的报文，reader 校验完整性、顺序，并统计丢失/重复/乱序/损坏；reader 可以在 callback 中 sleep 模拟慢 reader。Python 编排器负责多 lane 并发、随机 SIGKILL/重启 writer 与 reader、资源采样和进度/汇总。

## 构建

```sh
source /data/hxli/codes/tm_kit/build-common/build-env-setting.sh
meson setup builddir_stability_release test/shm_seqlock_stability_test/standalone --buildtype=release
ninja -j4 -C builddir_stability_release
```

集成构建（`test/shm_seqlock_system_test/suite`）也会生成 `shm_seqlock_stability_worker`。

## 运行

```sh
# 5 分钟冒烟，重启间隔按 0.1 倍缩短
python3 test/shm_seqlock_stability_test/run_stability.py \
  builddir_stability_release/shm_seqlock_stability_worker \
  --out=/tmp/shm-stability-smoke --minutes 5 --restart-scale 0.1

# 24 小时多路稳定性
python3 test/shm_seqlock_stability_test/run_stability.py \
  builddir_stability_release/shm_seqlock_stability_worker \
  --out=/tmp/shm-stability-24h --hours 24

# 跨 NUMA 性能基准（16 配置 × 5 轮，5s 预热 + 30s 测量）
python3 test/shm_seqlock_stability_test/cross_numa_bench.py \
  builddir_stability_release/shm_seqlock_stability_worker \
  --out=/tmp/shm-numa-perf --repeat 5

# 跨 NUMA 1 小时稳定性（4 lane，writer/reader 分节点绑核）
python3 test/shm_seqlock_stability_test/run_stability.py \
  builddir_stability_release/shm_seqlock_stability_worker \
  --out=/tmp/shm-stability-numa-1h --hours 1 \
  --lanes numa-same-w1-r1-4r,numa-cross-w0-r1-4r,numa-cross-w1-r0-1r,numa-cross-w0-r1-var4r

# 汇总 summary.json（可选附带 resource_monitor 输出目录）
python3 test/shm_seqlock_stability_test/summarize_stability.py \
  /tmp/shm-stability-numa-1h/summary.json /tmp/numa-stability.json \
  --resources /tmp/shm-stability-numa-1h-resources
```

lane 可配置可选的 `affinity`：`dict(writer='12', readers=['15','17',...])`，reader 列表按 reader index 循环取用；worker 进程以 `taskset -c` 前缀启动。worker 的 CLI 为 `worker role segment properties rate payload seed slowUs [durationMs [warmupMs]]`；设置 `warmupMs` 后，最终 STAT 只统计预热结束后的计数，延迟直方图也在预热结束时清零。

`--lanes` 可选逗号分隔的 lane 名；输出目录必须不存在。运行中每 `--sample-seconds`（默认 60s）写 `progress.json`，结束时写 `summary.json`；SIGINT/SIGTERM 会停止所有进程并写汇总。每个 worker 的原始 stdout/stderr 保存在对应 lane 目录。

## 默认 lane

| lane | 数据 | 频率 | reader | 布局/参数 | 重启 |
|---|---|---:|---|---|---|
| fixed-low-2r | 固定 64B | 100/s | 2 快 | inline, slotSize 512, slotCount 4096, futex | 无 |
| fixed-high-2r | 固定 64B | 100k/s | 2 快 | inline, 512/4096, spin | 无 |
| var-high-8r | 变长 64–4096B | 20k/s | 8 快 | split, 8192/1024, stride128, spin | 无 |
| ultra-var-max | 变长 64–4096B | **rate=0 最大吞吐** | 8 快 | split, 8192/1024, stride128, spin | 无 |
| fixed-4k-premark-4r | 固定 4KiB | 10k/s | 4 快 | inline-premark, 8192/512, read=spec | 无 |
| restarts | 变长 128–1024B | 2k/s | 4（含 1 慢） | inline, 2048/2048, futex | writer 120–300s；随机 reader 60–240s |
| slow-vs-fast | 固定 256B | 2k/s | 2 快 + 1 慢（5ms） | inline, 1024/4096, spin | 无 |
| tiny-ring | 固定 16B | 100/s | 2 快 | split, 128/2, spin | 无 |
| large-slot | 固定 1MiB | 100/s | 2 快 | inline, 2MiB/64, futex | 无 |
| mlock-writerlock | 固定 256B | 1k/s | 2 快 | inline, 512/512, mlock, writerLock | 无 |
| numa-same-w1-r1-4r | 固定 256B | 20k/s | 4 快 | writer NUMA1(13)、readers NUMA1(15/17/19/21)；inline 512/4096 spin | 无 |
| numa-cross-w0-r1-4r | 固定 256B | 20k/s | 4 快 | writer NUMA0(12)、readers NUMA1(23/25/27/3)；inline 512/4096 spin | 无 |
| numa-cross-w1-r0-1r | 固定 256B | 10k/s | 1 快 | writer NUMA1(5)、reader NUMA0(14)；inline 512/4096 futex | 无 |
| numa-cross-w0-r1-var4r | 变长 64–4096B | 20k/s | 4 快 | writer NUMA0(16)、readers NUMA1(1/7/9/11)；split 8192/1024 stride128 spin | 无 |

判定：所有 reader 的 `corrupt/bad/dup/ooo` 必须为 0；无重启 lane 的 fast reader 不允许丢失；慢 reader 的丢失只报告，不判失败；restart lane 的丢失允许，但必须能恢复且无损坏。writer 的 `rejected` 必须为 0。

## 外部资源监控

编排器自身只保存 RSS/PSS/线程/映射/FD 的最大值。要在不重启测试的情况下记录完整资源趋势，使用独立监控脚本（不发送任何信号、不启停被测进程）：

```sh
python3 test/shm_seqlock_stability_test/resource_monitor.py \
  --out=/tmp/shm-stability-72h-v2 \
  --monitor-out=/tmp/shm-stability-72h-v2-resources \
  --interval 30 --hours 72
```

- 通过 `PPID == runner PID` 只发现该批次启动的 worker；从 cmdline 解析 lane（segment 后缀）、role、reader index（seed 推导）、slow_us、rate、payload。
- 每 30s 采样每个 worker：RSS/PSS/VmSize/VmHWM/Swap、anon/file/shmem、线程、映射数与映射字节、FD 总数及 file/pipe/socket/anon_inode/dev_shm 分类、CPU%（utime+stime 差分）、voluntary/nonvoluntary 上下文切换、minor/major 缺页、`/proc/pid/io`（rchar/wchar/syscr/syscw/read_bytes/write_bytes）、smaps_rollup 明细。
- 每条记录带 `exp`（从 runner 的 `progress.json` 推导的实验时间轴）以及 `lane/role/reader_index/pid/seed/slow_us`，可与 runner 的 `summary.json`、per-instance 日志和 `progress_snapshots.jsonl` 关联。
- 输出：`resources.jsonl`（每 worker 每采样一条）、`progress_snapshots.jsonl`（runner 统计快照）、`resource_summary.json`（按 lane/role/index/PID 聚合 min/max/avg 与实例时间范围）。
- 注意：shm 数据面走 mmap，`/proc/pid/io` 主要反映控制面/日志 I/O；数据面压力应看 CPU、缺页、RSS/PSS 和映射。

## 跨 NUMA 实验

本机 NUMA0 的物理核为偶数 `0,2,…,26`，NUMA1 为奇数 `1,3,…,27`（HT 兄弟为 +28）。实验使用 NUMA0 `12,14,16,18,20,22,24,26` 与 NUMA1 `13,15,17,19,21,23,25,27`：

- `cross_numa_bench.py`：每轮独立启动 1 个 writer 进程和 N 个 reader 进程，各自 `taskset` 绑定到单核；35s（5s 预热 + 30s 测量）；placement 为 `same0`、`cross01`（writer NUMA0、reader NUMA1，段页在 NUMA0）、`same1`、`cross10`。默认 16 配置：1/8 reader × spin/futex × same0/cross01，反向 8 reader × spin/futex × same1/cross10，以及 4 个 `rate=0` 饱和配置。
- 相对比较只在同一批次内成立：worker 的 reader 会做逐字节 CRC 校验，且 `spinCount=1`，绝对延迟高于 `ResourceSystemTest` 的进程内测量，不能与性能矩阵的数值直接混用。
- `run_stability.py` 的 4 条跨 NUMA lane 在并发时必须使用**互不重叠**的物理核；启动时 `validate_affinity_disjoint` 会拒绝重叠的 CPU 列表。1h 实验用于验证跨节点长时间运行的数据完整性、丢失与资源趋势；runner 固定到 CPU 0、monitor 固定到 CPU 2，避免调度干扰测量核。

## 本轮结果

2026-11-14 09:09 至 11-16 09:50（48.7h，用户确认时长足够后停止）10 lane 全部 PASS：总发送 37,666,054,606 条，reader 累计接收 183,174,608,960 条，corrupt/bad/dup/ooo 全为 0；writer 重启 828 次、reader 重启 1147 次均恢复；资源 272,633 条采样，长期 PID 的 ΔRSS/FD/线程/映射均为 0。数据见任务记录的 `STABILITY_RESULTS.json` 与 `STABILITY_RESOURCE_RESULTS.json`。

跨 NUMA 补充（2026-11-16）：性能基准 16 配置 × 5 轮共 80 轮全部 PASS、零丢失（`PERF_NUMA_RESULTS.json`）；1h 稳定性 4 lane 全部 PASS、总发送 252,081,920 条、零丢失/损坏/乱序、资源零增长（`STABILITY_NUMA_RESULTS.json`）。

## 限制

- 稳定性轮次不校验 p99.9；延迟直方图为 64 bin/octave 的对数线性直方图，分位数为桶上界（相对误差 ≤1.6%）。
- 随机重启使用 SIGKILL，和 writer 崩溃/段重建场景一致；优雅停止使用 SIGTERM。
- 进程被 runner 杀死时，最后的 STAT 可能落后最多 1 秒。
- 本测试验证同一台机器上的组件行为，不覆盖跨机网络。
- 跨 NUMA 只覆盖两个节点上的选定物理核；未测试内存 interleave、`numactl --membind` 策略或跨 socket 之外的拓扑。
