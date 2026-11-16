# 真实 Heartbeat 与发现系统测试实施

**状态：** 完成。当前 HEAD 统一验收通过：**30 PASS** + 3 项 H04 风险探测（2×RISK_OBSERVED、1×RISK_NOT_OBSERVED，风险不计 PASS，详见下文）；证据 `/tmp/shm-system-leader-final2/heartbeat`。
**分支：** `feature/shm-system-heartbeat`
**开始：** 2026-11-13
**本子任务结束：** 2026-11-13
**代码提交：** 首轮 `006fa15`，补测 `02579d0`

主任务见 [README](README.md)，验收规格见 [PLAN](PLAN.md)。本记录由 Heartbeat 子 agent 维护，leader 汇总执行与总体状态。

## 实施方案与源码核对

- 使用独立 service/client 进程和 stdin/stdout 控制面。service 调真实 initializeHeartbeatAndAlertComponent、publishHeartbeat、sendAlert、sendCustomMessage；定时例调用 attachHeartbeatAndAlertComponent。
- client 使用真实 MultiTransport heartbeat importer 与 setupBroadcastListenerThroughHeartbeat 发现业务广播，逐条校验业务内容。
- 源码 `MultiTransportBroadcastListenerManagingUtils::AddSubscriptionCreator` 只有 seenLocators 去重和追加订阅，没有 TTL 删除或地址替换。按真实语义验证新地址追加和旧地址继续可用，不把“旧地址删除”当契约。
- TTL 通过真实 HeartbeatMessageToRemoteFacilityCommandConverter 消费网络收到的 heartbeat；注册一个 socket_rpc locator 作为转换器输入元数据，测试 Register/Deregister 指令，不启动 RPC 服务、不宣称验证了 RPC 连接。
- 初始化和映射分别用控制命令与真实数据探针确认；固定 sleep 只用于明确的静默/TTL 时间窗口，不作为 reader 就绪判据。
- 独立目标直接编译所需组件源文件，不改生产实现、不带故障注入。

## 测试与结果

以下按阶段记录；最终实际结果见 Debug 与 Release 验证章节。

## 构建进展

- 工作区根 Meson setup 在 grpc/libetcdcpp 的 pkg-config 展开耗时过长，未修改第三方或系统设置。增加测试目录的 `standalone/meson.build`，只解析 basic、protobuf、uuid、threads，Meson 1.11.2 / GCC 15.2.1 配置成功。
- 首轮 `ninja -j1 -C builddir_heartbeat_debug` 已编译完成；所有子 agent 同时编译时各 `-j1`。
- Python 驱动通过语法检查；每阶段 5 秒、每例外部 watchdog 30 秒，保存进程 status/wchan/stack（权限不足也记录），只终止登记的 worker。

- Debug 首次编译成功；`direct`/`inline` 冒烟通过（字段、真实发现后的 20 条业务、alert/custom、10 次重复心跳不产生重复业务订阅；2 个子进程均退出 0）。日志 `/tmp/shm-heartbeat-smoke/inline-direct/`。后续完整 Debug 矩阵结果见下节。

## Debug 验证结果

2026-11-13，GCC 15.2.1 / Meson 1.11.2，独立 `builddir_heartbeat_debug`，无注入构建。

```sh
source /data/hxli/codes/tm_kit/build-common/build-env-setting.sh
meson setup builddir_heartbeat_debug test/shm_seqlock_heartbeat_system_test/standalone --buildtype=debug --prefix="$INSTALL_PREFIX"
ninja -j1 -C builddir_heartbeat_debug
python3 test/shm_seqlock_heartbeat_system_test/run_tests.py builddir_heartbeat_debug/shm_seqlock_heartbeat_system_worker --output /tmp/shm-heartbeat-debug-all
```

- 9 类用例 × inline/split/inline-premark = **27 PASS / 0 FAIL**；进程退出码符合预期（SIGKILL 例 -9，锁冲突例 1，其余 0）。
- 编译日志 `/tmp/shm-heartbeat-final-build.log` 无 `error:` / `FAILED:`，存在既有头文件弃用等警告。
- 原始逐进程日志和结果 `/tmp/shm-heartbeat-debug-all/`；归档摘要见 [heartbeat-debug-results.json](heartbeat-debug-results.json)。
- 最后 Python 资源关闭收尾调整后，`direct/inline` 重新冒烟通过；`python3 -m py_compile` 与 `git diff --check` 通过。

## 覆盖与剩余事项

测试目录 [README](../../test/shm_seqlock_heartbeat_system_test/README.md) 给出每例与 PLAN H01–H09 的映射。实际覆盖真实 initialize/publish/attach、字段和 topic 类型隔离、真实发现业务链路、地址追加、TTL 转换器、心跳恰满和超一、SIGKILL 接管和死 writer 残留、SIGSTOP 锁冲突。

首轮未执行 H04、reader 慢于 TTL 和 Release；本轮后续已补齐这些项目，结果见下。饱和数据覆盖心跳、真实长跑及效率分位仍由主任务独立推进。广播发现无 TTL 删除是源码既有语义，不能写为缺陷或虚构通过。仅增加测试，不修改 API、布局或生产行为。

## 后续补测（2026-11-13）

leader 已集成首轮提交，要求继续覆盖 H06 慢 reader、H09 停止后恢复、H04 隔离风险及完整 Release 矩阵。继续沿用同一任务记录。

- 慢 reader 采用真实 graph heartbeat callback 的独立控制屏障；阻塞超过 TTL 后从独立主线程触发真实 converter CHECK，确认过期，再释放 callback 验证接收时刻更新及恢复。
- 短 furthestPoint 停止后显式 publishHeartbeat 恢复，再证明 expired timer 不会自行恢复。
- H04 使用 first-unlocked cached sender，再按 writerLock=true 初始化 Heartbeat，两个线程并发发送；只报告 OBSERVED / NOT_OBSERVED，不报告 SAFE。
- 审查发现首轮 close 未校验最后一条断言之后异步写出的 BAD 记录/多余 DATA；本轮收紧 final drain，避免尾部错误漏检。生产实现不修改。

- 补充 oracle 审查：死 writer 残留例的客户端 READY 早于 mmap；现在先关闭所有旧 reader，等待残留段真实 futex waiter 登记（头部源码约定 offset=512），再启动静默断言，防止尚未映射造成假通过。
- 为 final-drain 增加独立负对照 `verify_driver.py`：正常尾部接受，QUIT 之后才写出的 DATA/BAD_HB/ERROR/REGISTER 均必须拒绝；5 项已通过。

## Release 补测结果与 oracle 审查

2026-11-13，GCC 15.2.1 / Meson 1.11.2，独立 `builddir_heartbeat_release`，无故障注入。

```sh
source /data/hxli/codes/tm_kit/build-common/build-env-setting.sh
meson setup builddir_heartbeat_release test/shm_seqlock_heartbeat_system_test/standalone --buildtype=release --prefix="$INSTALL_PREFIX"
ninja -j1 -C builddir_heartbeat_release
python3 test/shm_seqlock_heartbeat_system_test/run_tests.py builddir_heartbeat_release/shm_seqlock_heartbeat_system_worker --output /tmp/shm-heartbeat-release-final
python3 test/shm_seqlock_heartbeat_system_test/verify_driver.py
```

- **30 个支持用法 PASS / 0 FAIL**：10 类 × 3 layouts。H06 callback 屏障实际阻塞约 1.151s，超过 1s TTL，旧接受状态过期、释放后重新 Register、业务恢复全部通过。
- H09 同段与分段 timer 例都验证真实 attach 定时发布、短 furthestPoint 后静默、converter 过期、显式 publishHeartbeat 恢复，以及 expired timer 不自行重启。
- **3 个 H04 风险单列，不计入 PASS**：最终完整轮 inline-premark 观察到非法/逆序，inline/split 未观察到。再次采样风险证据轮在 split 和 inline-premark 分别观察到 2 条异常；首轮完整 Release 在 inline 观察到 1 条异常。三布局均在至少一轮出现过风险，结果有随机时序性，“未观察到”不表示安全。
- 最终完整结果见 [heartbeat-release-results.json](heartbeat-release-results.json)，附首个异常字节前缀的独立风险证据轮见 [heartbeat-risk-results.json](heartbeat-risk-results.json)。原始日志 `/tmp/shm-heartbeat-release-final/`、`/tmp/shm-heartbeat-release-risk-evidence/`；最初 Release 轮为 `/tmp/shm-heartbeat-release-all/`。
- final-drain 负对照 **5 PASS**：正常尾部接受；QUIT 后才出现的 DATA、BAD_HB、ERROR、REGISTER 均被拒绝。摘要见 [heartbeat-driver-oracle.json](heartbeat-driver-oracle.json)。
- 编译日志 `/tmp/shm-heartbeat-release-build.log` 无 `error:` / `FAILED:`。Python 语法检查和 `git diff --check` 通过。
- 补测代码未重新跑 Debug；首轮 Debug 的 27 PASS 只对应首轮代码，不能计为新 slow-reader / risk 的 Debug 验证。leader 集成后可统一执行。

审查修正均限 fixture：增加 raw heartbeat 解码校验防 typed 静默丢弃、关闭时完整 drain 检查尾部坏消息/多余交付/未消费注册事件、验证每条 timer heartbeat 字段、死 writer 残留先确认 futex 映射登记再判静默。控制屏障在 Env 析构前解除，保持 converter 和 runner callback 所有者存活至 transport 线程停止；只在全部登记 worker 退出后删除段/锁文件。
