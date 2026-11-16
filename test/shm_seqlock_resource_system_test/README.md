# ShmSeqlock 资源与性能系统测试

任务记录：[RESOURCE.md](../../doc/20261113-shm-seqlock-system-test/RESOURCE.md)。

本程序直接编译生产组件源码，编译时拒绝故障注入宏。独立构建：

```sh
source /data/hxli/codes/tm_kit/build-common/build-env-setting.sh
meson setup builddir_resource_release test/shm_seqlock_resource_system_test/standalone --buildtype=release
ninja -j1 -C builddir_resource_release
```

## 程序入口

```sh
BIN=builddir_resource_release/shm_seqlock_resource_system_test
"$BIN" --mode=oracle
"$BIN" --mode=perf --readers=2 --payload=256 --layout=inline --read=classic --wait=futex --rate=10000 --warmupMs=5000 --durationMs=30000
"$BIN" --mode=perf --readers=2 --rate=0 --verify=true --warmupMs=1000 --durationMs=1000
"$BIN" --mode=churn --cycles=10000 --wait=futex --selfRemove=false
"$BIN" --mode=churn --cycles=10000 --wait=futex --selfRemove=true
```

- `oracle`：逐字节破坏 128 字节合法报文，必须全部检出；验证延迟直方图边界。
- `perf`：reader 数 0..8、payload 48B..1MiB（含测试头，不含 2B topic），`rate=0` 为饱和。总段尺寸限制 64MiB。支持 `slotCount`、`stride128`、`mlock`、`writerLock`、`spinCount`。开测前用真实交付确认各 reader，测量期间按预定时刻开放式发送，结束后单独排空。
- `verify=true`：逐字节模式与校验和校验。默认性能轮次只校验头、topic、顺序；输出明确区分两种轮次。
- `churn`：外部 remove 与 callback 自 remove 分开，10,000 循环默认每 1,000 次采样。组件仍存活时映射持续积累判 FAIL，析构释放另报。RSS、PSS、FD、映射、线程、CPU、缺页与切换均记录。资源轮次使用重复控制报文保障就绪，不能用它测延迟。
- 输出 JSON Lines。延迟直方图保留全部样本，最多约 1.6% 桶上界误差；不足百万样本不输出 p99.9。丢失率包括测量开头/尾部缺口；transport lost 另报并注明包括预热，避免混作精确等价。

## 编排入口

```sh
python3 test/shm_seqlock_resource_system_test/run_tests.py "$BIN" --profile=smoke --out=/tmp/shm-resource-smoke
python3 test/shm_seqlock_resource_system_test/run_tests.py "$BIN" --profile=churn --cycles=10000 --out=/tmp/shm-resource-churn
python3 test/shm_seqlock_resource_system_test/run_tests.py "$BIN" --profile=full --out=/tmp/shm-resource-full
python3 test/shm_seqlock_resource_system_test/run_tests.py "$BIN" --profile=soak --hours=2 --out=/tmp/shm-resource-2h
python3 test/shm_seqlock_resource_system_test/run_tests.py "$BIN" --profile=soak --hours=24 --out=/tmp/shm-resource-24h
```

输出目录必须尚不存在。`full` 默认 5s 预热、30s 测量、5 次，随机打乱 0/1/2/4/8 readers × 64/256/1024/4096B × layout/read/wait 矩阵。完整矩阵约需 12 小时；可用 `--readers`、`--payloads`、`--layouts`、`--reads`、`--waits` 缩小维度，`--rate=0` 测饱和，`--max-rounds` 明确标记缩短矩阵。不同 offered rate 的校准和 50%/90% 负载由操作者提供，不自动声称已经校准。`performance_baseline` 只在全部默认维度且 rate=10000、repeat>=5、无失败时置 true；子集批次为 false，需在报告中明确标注为 staged 基线。

`soak` 保持同一组组件持续运行指定时长，每分钟输出资源快照。参数列表选最后一个值，可明确提供单值选择运行配置。它不等价于写者崩溃/布局切换混合长跑；这些由系统故障驱动组合执行。

保存编译器/buildtype、提交/脏状态、二进制 SHA256、CPU/NUMA/频率/亲和性、参数、每进程退出码和原始 stdout/stderr。watchdog 超时保存 `/proc` 线程栈/等待点/映射后杀死本进程组，再精确清理登记的段和锁文件。正式 full/soak 仅接受 release。性能差异需与同机历史两批复测比较；本入口不把首轮短冒烟认作性能回归结论。

## 已知失败

基线组件的 callback 自移除路径把 subscription 放进 `stoppedSubscriptions_`，直到组件析构才 join/释放其映射。此测试保留 FAIL，不能把最终析构释放视为组件存活期间无积累。
