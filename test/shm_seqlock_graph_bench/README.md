# ShmSeqlock 图 / MultiTransport 性能基准

任务记录：[README](../../doc/20261113-shm-seqlock-system-test/README.md)。

本程序测量 **RealTimeApp 图路径** 下的 shm_seqlock 性能：触发导入器（trigger importer）→ 发布节点 → 共享内存 → 导入器/监听节点 → 接收节点（sink）。两种模式：

- `raw`：直接使用 `ShmSeqlockImporterExporter`。
- `multi`：使用 `MultiTransportBroadcastPublisherManagingUtils` / `MultiTransportBroadcastListenerManagingUtils` 的 shm_seqlock 通道。

同一进程内回环；`raw` 与 `multi` 的差值反映 MultiTransport 管理层的开销。消息含 generation、序号、发送时间、长度和 FNV-1a 校验和；接收节点逐条校验并记录“发送→回调”的延迟直方图（64 bin/octave，分位数为桶上界，相对误差 ≤1.6%）。

## 构建

```sh
source /data/hxli/codes/tm_kit/build-common/build-env-setting.sh
# 方式一：与系统测试共用集成工程
ninja -j4 -C builddir_suite_release shm_seqlock_graph_bench
# 方式二：独立工程
meson setup builddir_graph_bench test/shm_seqlock_graph_bench/standalone --buildtype=release
ninja -j4 -C builddir_graph_bench
```

## 运行

```sh
# 单轮：raw|multi  段名  监听数(1..8)  目标速率(0=饱和)  payload字节  测量ms  预热ms  [layout] [read] [wait]
builddir_suite_release/shm_seqlock_graph_bench raw benchseg 1 10000 256 30000 5000 inline classic spin
builddir_suite_release/shm_seqlock_graph_bench multi benchseg 4 10000 256 30000 5000 inline classic spin

# 5 轮完整批次（12 配置 × 5 轮）
python3 test/shm_seqlock_graph_bench/run_bench.py \
  builddir_suite_release/shm_seqlock_graph_bench \
  --out=/tmp/shm-graph-bench --repeat 5
```

单轮输出一行 JSON：`offered`（测量窗口内发送条数）、`sinks[]`（每个接收节点收到的条数、丢失/重复/乱序/损坏计数、p50/p90/p99/max 延迟）。

## 默认配置（run_bench.py）

| 维度 | 取值 |
|---|---|
| 模式 | raw、multi |
| 监听节点 | 1、4 |
| 速率 | 10k 条/s（spin、futex）、rate=0 饱和（spin） |
| payload | 256 字节（另加 40 字节测试头） |
| 每轮 | 5 s 预热 + 30 s 测量，共 5 轮，随机顺序 |

## 判定与注意

- 10k 条/s 配置要求 `lost/dup/ooo/corrupt/bad` 全 0；`rate=0` 饱和配置允许丢失，单独报告。
- 发布者与监听节点在同一进程内，测的是图路径与共享内存的单机回环，不包含跨机网络。
- 延迟绝对值只在本程序内可比；与 `ResourceSystemTest`（直接组件、不同校验开销）的数字不可混用。
- 本基准不绑定 CPU 核，主机共享；需要绑核时用 `taskset` 包住整条命令。
