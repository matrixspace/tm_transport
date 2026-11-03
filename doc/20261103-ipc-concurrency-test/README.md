# IPC Transport 并发测试

**任务开始时间：** 2026-11-03  
**状态：** 完成

## 背景

基于之前对 shm_seqlock、shm_cursor、aeron_ipc 多线程发布风险的分析（见主对话），
为 tm_transport 中支持 IPC 的四种 transport 新增系统性并发测试程序。

## 覆盖的 transport

- `shm_seqlock`
- `shm_cursor`
- `aeron_ipc`（需 Aeron 依赖，构建时自动检测）
- `shared_memory_broadcast`

## 测试文件

```
test/ipc_concurrency_test/
├── ConcurrencyCommon.hpp       # 报文格式(PayloadHeader/FNV-1a校验)、选项解析、Checker类
├── TransportAdapter.hpp        # 四种 transport 的统一 API 封装（getPublisher/subscribe/stats）
├── IpcConcurrencyTest.cpp      # 测试程序主体（inproc / pub / sub / graph 四种模式）
├── meson.build                 # 构建配置
└── run_tests.sh                # 驱动脚本，覆盖 10 类测试场景
```

## 测试场景

| 编号 | 场景 | 类型 |
|------|------|------|
| 1 | 进程内多线程读（3 comp × 3 client + churn） | PASS/FAIL |
| 2 | 进程内多线程写，启用传输层锁属性 | PASS/FAIL |
| 3 | 进程内多线程写，不加锁，各自调 getPublisher | RISK/SAFE |
| 4 | 进程内多线程写，不加锁，共享一个 publisher | RISK/SAFE |
| 5 | 锁属性只在部分 locator 上设置（首个 locator 生效） | RISK/SAFE |
| 6 | graph：多个 threaded importer → threaded=false exporter | RISK/SAFE |
| 7 | graph：多个 threaded importer → threaded=true exporter | PASS/FAIL |
| 7b | aeron_ipc：单 writer，默认 untethered 读端 | RISK/SAFE |
| 8 | graph threaded=true + 模拟心跳的额外直接 publisher | RISK/SAFE（不加锁）/ PASS/FAIL（加锁） |
| 9 | 多进程读：1 pub 进程 + 3 sub 进程 | PASS/FAIL |
| 10 | 多进程写：2 pub 进程，同一 segment/stream | transport 专项 |

`PASS/FAIL` = 支持的用法，任何撕裂/混淆/乱序/重复消息即判 FAIL。  
`RISK/SAFE` = 不支持的用法（如未加锁并发写），验证问题是否可重现。  

## 全量运行结果（release 构建，2026-11-03）

```
SUMMARY pass=18 fail=0 risk=14 safe=7
```

RISK 项全部符合预期（对应之前分析的已知风险）；所有 PASS/FAIL 项均为 PASS。

## 已知问题（记录在测试脚本中，非本任务引入）

### aeron_ipc — untethered 读端 term 重用（测试 1、7b）

untethered 读端（默认配置）在读端滞后时，publisher 可能已回收正在读的 term，
导致读端拷贝到损坏数据（`corrupt` 计数非零）。与多线程写无关。  
**规避：** 测试 1 和 7 对 aeron_ipc 使用 `tether=true`；7b 单独 probe 此行为。

### shared_memory_broadcast — 多读端组件时 publisher 侧崩溃（测试 1、7）

`OneSharedMemoryBroadcastSender::publish` 遍历 clientListHead_ 链表时无跨进程锁，
多个 subscriber 组件（各自 Env，各自 shared memory record）并发 open/close 会触发
publisher 侧 use-after-free，SIGSEGV。  
**规避：** 测试 1 对 shared_memory_broadcast 标记为 RISK；测试 7 使用单 reader comp（1×2）。

## 构建

通过 meson 自动加入构建，编译只需：

```bash
cd build-common && source ./build-env-setting.sh
source /opt/rh/gcc-toolset-15/enable
cd ../tm_transport
ninja -j4 -C builddir_release test/ipc_concurrency_test/ipc_concurrency_test
```

## 运行

```bash
# 全部 transport
LOGDIR=/tmp/ipcc bash test/ipc_concurrency_test/run_tests.sh \
    builddir_release/test/ipc_concurrency_test/ipc_concurrency_test

# 只跑指定 transport
LOGDIR=/tmp/ipcc_sl bash test/ipc_concurrency_test/run_tests.sh \
    builddir_release/test/ipc_concurrency_test/ipc_concurrency_test shm_seqlock
```
