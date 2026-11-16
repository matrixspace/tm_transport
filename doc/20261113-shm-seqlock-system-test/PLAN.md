# ShmSeqlockComponent 系统测试方案

方案基线：`tm_transport ccb9a8c`，2026-11-13。任务状态与执行记录见 [README](README.md)。这是实施规格，不是执行结果。

## 1. 目标与判定原则

覆盖组件直接调用、实时数据流图、MultiTransport、真实 Heartbeat 与服务发现。复用 `shm_seqlock_test`、`ipc_concurrency_test` 的校验和驱动，补齐缺口。现有测试通过不替代新增验证。

| 类别 | 判定 |
|---|---|
| 支持用法的正确性 | 损坏、非法重复、逆序、死锁、崩溃均失败；允许有损时按规则核算 |
| 故障恢复和防御能力 | 验证恢复、安全性、资源释放；实现不足记录缺陷，不降低断言 |
| 不支持用法风险探测 | 未加锁并发写、外部篡改等；未复现只记“未观察到”，不能判安全 |

现有语义必须体现在 oracle 中：

- 单段是有损广播，慢 reader 不向 writer 施加队列背压。
- 每组件每段一条读线程，同段订阅不隔离慢 callback。
- 同组件重复获取同段 publisher 复用 sender，首次创建属性生效。
- writerLock 只串行化该 sender，不能据此允许不同进程同时写同段。
- received 是过滤前读取计数，不等于单个 callback 的交付数。
- corrupt 不代表 payload 校验失败数；必须另设内容校验。
- join=latest 仅在探测到 publisher 存活时交付已有最新消息。
- 传输节点只用于 RealTimeApp。业务逻辑可用 mock/file importer 做回测，不把共享内存节点接入单遍执行器。

## 2. 测试设施与接口

### 2.1 组织

在现有测试体系扩展以下职责，避免重新实现一套传输封装：

- 场景协调器：启动 writer、reader、图服务及故障进程，执行屏障、暂停/恢复、kill 和 watchdog。
- 校验器：原始字节、typed、topic、hook 与统计对账。
- 故障夹具：残留段、异常头部、指定崩溃点和受控资源故障。
- 性能驱动：与正确性轮次分开，减少日志和校验对热路径的影响。
- 汇总器：输出 JSON 与人工报告，保存失败 seed、完整参数、子进程退出状态。

复杂编排用 Python，构建和简单入口用 shell。控制面通过独立管道或 Unix socket，不依赖被测通道。每例唯一段名；只清理登记的 PID、段和锁文件，活跃 writer 存在时不得删除锁文件。

### 2.2 校验与同步

普通报文包含运行 ID、writer 代次、writer 线程 ID、线程内序号、长度、发送时间、校验值和确定性填充。覆盖 topic 和 payload，不只校验固定头部。

- 单 writer 连续订阅内严格递增，无重复。
- 多线程加锁要求每线程序号递增；调用前分配的全局序号不能代表提交顺序。
- 小/空报文用预先登记的期望字节串，避免测试头占用容量。
- 重订阅、换段、writer 重启分别划区间；latest 重订阅重放最新值不误报重复。
- lost 只在无过滤、无拒绝、加入完成、无换段的受控区间与序号缺口精确对账。
- 跨重启/换段分别记录统计，不假设终生累计，不要求多字段快照事务一致。

分别建立“进程启动”“订阅 API 返回”“reader 已映射并确定起点”握手；固定 sleep 不代替就绪屏障。普通阶段默认 5 秒、单例默认 30 秒，慢 callback 和长跑显式扩大预算。外部 watchdog 超时保存线程栈、阶段、统计和子进程状态。

### 2.3 测试专用接口

已获用户同意，增加编译期开关控制的故障注入点：生产构建无注入分支、等待或 ABI 改动。注入点经独立控制通道报告到达，再等待继续或由协调器 SIGKILL。提供映射完成、写入、重建、futex 登记等事件。

不增加公共业务 API，不改共享内存布局。生产构建另跑黑盒随机故障，避免测试时序掩盖问题。

## 3. 功能与框架集成

### 3.1 参数矩阵

核心收发、套圈、恢复遍历 3 layout × 2 read × 4 writer/reader wait 组合，共 24 组：

- layout：inline、split、inline-premark。
- read：classic、spec。
- wait：spin/spin、futex/futex、spin/futex、futex/spin。

其他属性做边界和成对覆盖：slotCount=2/4/64/4096，stride128 与 mlock 开关；spinCount=0/1/默认（验证 0 归一化），pauseAfter=0/1/代表性正值，join=latest/next。

消息大小覆盖 0、1、39/40/41、63/64/65、319/320/321，以及容量减一/恰满/超一；topic/payload 独立变化。最大参数不能直接申请最大容量乘最大槽数，采用资源受控构造和异常参数验证。

### 3.2 功能用例

| ID | 操作 | 核心验收 |
|---|---|---|
| F01 | 单 writer、多进程 reader，各种启动先后；无 reader 发布 | 启动可靠；受控低速无丢失；无 reader 不阻塞 |
| F02 | 空、二进制、NUL、长 topic；长度交替；跨缓存行 | 字节一致；重点验证 spec 预测和 inline-premark 分段写 |
| F03 | 恰满、超限、hook 扩展后超限；合法非法交替 | rejected 精确；拒绝不破坏后续消息 |
| F04 | 无过滤、精确、regex、MultiTransport topic 语法，同段多过滤器 | 交付集合正确；过滤不算丢失 |
| F05 | hook 往返、nullopt、默认 typed hook、显式 hook | 内容/次数正确；丢弃后后续继续 |
| F06 | typed 往返、非法编码、错误类型、截断 | 解码失败不进业务 callback；后续合法值正常 |
| F07 | latest/next：空段、活 writer 静止、死 writer 残留、新 writer 接管 | 第一条精确；不重放死 writer 旧最新值 |
| F08 | 首次/后续属性冲突；不同组件同段；多段隔离 | 首次属性规则；读端遵循段头；不同段隔离 |
| F09 | 非法名字/layout、数值越界/负数/尾随字符、非 2 次幂、未知属性 | 区分拒绝和现有宽松解析；失败后可有效创建 |
| F10 | 统计/线程句柄查询与收发增删并发 | 无崩溃死锁，按实际生命周期解释计数 |
| F11 | CPU provider 成功/失败/重复设置/缺失、冲突、租约释放 | 异常传播；无半启动缓存；后续有效订阅成功 |

### 3.3 图与非图

| ID | 路径 | 验收 |
|---|---|---|
| G01 | 直接组件，无 Environment/AppRunner | 原始字节、hook 全功能 |
| G02 | ShmSeqlockImporterExporter<Env> raw/typed 图 | finalize 启动、实际通过、显式保活、安全退出 |
| G03 | MultiTransport publisher/listener，图与直接 API 互通 | channel、topic、hook、typed 一致 |
| G04 | 同步 runner 管理路径、动态 listener 管理 | 加入、移除、替换、生命周期 |
| G05 | 多上游线程汇入 exporter | 非线程化 exporter 加 writerLock；线程化串行路径分别验证 |
| G06 | 图 exporter 与额外直接 publisher 共段 | exporter 自身串行仍需首次启用 writerLock |
| G07 | importer stop/restart、重复命令和交错 | 等待异步控制完成；无多重订阅/死锁/悬垂 |
| G08 | raw/typed fetch-first，过滤/predicate 不匹配、非法后合法 | future 完成一次且退订；未匹配不误完成 |
| G09 | 图中慢同步/异步下游 | 区分 transport 套圈与图队列积压 |

退出保持 Env、节点和 callback 所有者存活至异步动作结束。不假定未完成 future 有内置超时/取消；由外部 watchdog 结束。回测只验证共享业务逻辑，不能把不支持的传输实例化算功能需求。

### 3.4 真实 Heartbeat 与发现

调用真实 initializeHeartbeatAndAlertComponent、publishHeartbeat、attach 和发现工具，不能只模拟心跳字节。

| ID | 场景 | 验收 |
|---|---|---|
| H01 | 非图显式发布；图定时发布 | UUID、PID、服务名、时间、状态和 channel 列表 |
| H02 | Heartbeat/alert/custom message 共段 | topic/类型隔离；并发首次启用 writerLock |
| H03 | 数据与心跳共段加锁；独立段对照 | 无撕裂；测间隔、数据覆盖心跳风险、发现稳定性 |
| H04 | 首次未加锁，后续心跳 locator 才加锁 | 风险探测：不能补救已有 sender |
| H05 | 发现后订阅实际业务；服务重启、地址更新 | 完整链路；更新/过期/重连按工具真实语义 |
| H06 | writer 崩溃、长停顿、reader 慢于 TTL | 过期与恢复；死 writer 残留不让新客户端误上线 |
| H07 | 心跳恰满/超限，状态/channel 列表增长 | rejected 可观察；不混淆容量与发现故障 |
| H08 | 两服务进程误用同段；每服务独立段对照 | 第二 writer 明确失败；独立段汇聚监听成功 |
| H09 | 短 furthestPoint，显式长运行配置 | 定时停止、服务过期；长跑覆盖实际时长 |

默认周期 100ms、TTL 1s、检查周期 200ms，按消费者最后接受的心跳和检查周期判过期，另留 500ms 调度余量。不假定所有 Heartbeat 工具都有相同 TTL 删除行为，先读实现再选对应断言。

## 4. 鲁棒性与恢复

### 4.1 慢 reader 和生命周期

| ID | 注入 | 验收 |
|---|---|---|
| R01 | callback 屏障暂停，writer 精确推进 N−1/N/N+1/2N+1 条 | 按 layout 验证套圈边界、最新恢复与 lost；无撕裂 |
| R02 | 不同组件/进程快慢 reader 共存 | 慢端不阻塞 writer；快端在校准负载不丢 |
| R03 | 同组件同段一个慢、多个快 callback | 验证共享线程拖慢，不错误要求隔离 |
| R04 | reader SIGSTOP，writer 绕多轮，再 SIGCONT | 恢复新鲜数据；无逆序/死锁/无限重试 |
| R05 | 高频增删，最后两订阅并发移除，移除/加入并发 | 无 UAF、锁序死锁、误删新对象 |
| R06 | callback 自移除/移除其他/重新订阅 | 允许当前 dispatch 已捕获 callback，后续无旧订阅 |
| R07 | 最后订阅退出再加入；未知 ID/重复 remove | 安全；重新选择首次属性 |
| R08 | 频繁创建/退出/SIGKILL，writer 频繁接管 | 10,000 订阅循环、1,000 进程重连；资源趋势 |

重点监测 stoppedSubscriptions_：自移除对象延后保存可能导致组件仍存活时资源积累；分别报告积累和析构释放，不能只看进程退出后的释放。

### 4.2 writer 精确崩溃窗口

统一步骤：reader 就绪 → writer 到达注入屏障 → SIGKILL → 确认退出 → 新 writer 接管 → 发新代次消息 → 校验。全 layout、全 read 覆盖：

1. 持锁后、初始化完成前。
2. 新段创建后、有效 magic 前。
3. slot 标 odd 后。
4. topic/长度写入中、payload 写入中。
5. inline-premark 冷数据写完、首缓存行未提交。
6. slot 已 even、header published 未更新。
7. published 已更新、下一 slot 未 premark。
8. premark 已完成、futex 未唤醒。
9. 旧段 superseded、新段未就绪。
10. 同布局恢复扫描和 epoch 更新中再次崩溃。

断言：未提交部分报文不交付；已 even 未更新 published 的完整报文可能已交付，恢复不能复用索引造成重复/回退；odd 残留不永久卡 reader；无新 writer 可等待但必须可停止。

同布局接续与不同布局重开分开对账；换段以新运行代次区分新序号空间。分别切换 layout/count/size/stride，layout 做全部有向转换。重启后持续流量再转 idle，允许 restart 统计到 idle 才观察更新。

默认恢复期限：新 writer 就绪后 5 秒内收到验证消息，另报告延迟分布；不能以“最终收到”替代通过。

### 4.3 多 writer 与 futex

- 2/8 独立进程同时抢段，最多一个成功；失败者明确锁冲突、不污染段。
- 同进程不同组件竞争同段，验证独立 open 锁文件的实际互斥。
- writer 正常退出/SIGKILL 后可接管；SIGSTOP 仍持锁，不可夺取。
- 同组件多线程共享/分别获取 publisher，writerLock=true 保完整性和拒绝计数。
- 未加锁并发写仅做隔离风险探测。
- futex 登记前后、睡前后、唤醒前后交错，无永久丢唤醒。
- kill 睡眠 reader，验证泄漏 waiters 经连续空唤醒清理。
- 清理时仍有活跃 futex reader，饱和递减不下溢，超时后继续交付。
- 重启切换 spin/futex；reader-only futex 走回退轮询。
- 无 writer、空段、futex 等待时析构；停止耗时分别解释 timeout 和 callback 阻塞。

### 4.4 脏段、环境与边界

隔离子进程构造：零长度/短头、未完成 magic、未知 layout、非法 count/stride/capacity、长度不匹配；odd、超长长度字段、错误 topicHash、异常 published/seq；权限/FD 限额、mlock/mmap/创建失败；unlink/recreate、活跃映射 truncate、外部替换锁文件；callback/hook 抛异常或永不返回；publisher 捕获函数超出组件生命周期；futexWord 溢出附近及序号极值。

保证边界：正常崩溃残留属于必测恢复；任意篡改已提交 payload 不由 seqlock 提供完整性检测。非法头理想为拒绝/等待，越界、崩溃、无限分配报告防御缺陷。外部 truncate/锁文件替换/非法生命周期单列风险，不宣称已有安全保证。耗尽资源只在私有受限环境执行，条件不足明确 SKIP，不能填满公共 /dev/shm。

## 5. 效率

使用无故障注入的 release，记录 CPU/NUMA/亲和性、编译器、提交、内核、频率策略和实际参数。

指标：publish 耗时；发送前至 callback/图 sink 的端到端 p50/p90/p99/p99.9/max；同时报告丢失率/新鲜度；offered/accepted/received 吞吐、拒绝/套圈率；进程/线程 CPU、上下文切换、缺页、RSS/PSS、FD/映射/线程数；重连/重映射/接管时间；futex 空闲 CPU、唤醒延迟。

维度：reader 0/1/2/4/8（不足明确缩减）；payload 64B/256B/1KiB/4KiB（容量含 topic）；低频、校准无丢失吞吐 50%/90%、饱和/突发；layout/read/wait/stride/mlock/writerLock；直接/图/MultiTransport、真实心跳共段/分段；独立慢 reader/同线程慢 callback；独立核/同核争用、有 NUMA 时同/跨 NUMA。

每组预热 5s、测量 30s、至少 5 次、随机顺序。p99.9 至少 100 万样本，不足只报较低分位和样本数。用同机单调时钟，按预定时间发，不用 reader ACK 控速掩盖延迟。内容校验另跑，性能轮次保留低成本序号检查。

固定测试机初始门槛：

- 同机基线吞吐降 >10% 或 p99 升 >20% 触发复测，两批稳定复现判回归。
- 独立慢 reader 下 writer 平均耗时沿用现有 1.5×基线+50ns，另检查尾延迟。
- futex 低频目标单核 CPU <5%，连同 spin 对照报告。
- 无业务 SLA 不虚构绝对 ns 门槛，首次固化版本化基线。

## 6. 执行与验收

| 层级 | 内容 |
|---|---|
| 每次修改 | debug/release 核心、24 组核心参数、图/Heartbeat 冒烟、关键精确窗口 |
| 夜间 | 完整故障矩阵，每确定性窗口重复 20 次，固定/随机 seed churn，2h 压力 |
| 发布前 | 全量、独立性能批次、24h 混合长跑、资源故障、ASan/UBSan |
| 诊断 | TSan 查同进程生命周期/管理；不能代替跨进程共享内存正确性验证 |

构建顺序：source 工作区 build-common/build-env-setting.sh → 确认 gcc-toolset-15 → 本 worktree 独立 Meson setup（--cmake-prefix-path 使用 VCPKG_ROOT）→ ninja -j4 相关目标。不同 buildtype/sanitizer 独立目录，不复用原 checkout 缓存。

先执行现有 shm_seqlock 三种 layout 和 IPC 并发中的 shm_seqlock 基线。新增驱动支持 case、seed、layout、read、wait、重复次数和日志目录选择。

负对照 testNoPremark=true 检验撕裂敏感性，未观察到记“不充分”，不随机要求每次必现；另用确定性错误报文验证校验器必报错。

验收与交付：

- 每例有 ID、前置、步骤、断言、超时、清理；保存参数矩阵、入口、环境、原始结果、性能基线和缺陷。
- 支持用法全通过，校验零错误，无非预期崩溃/死锁/生命周期错误。
- 允许丢失有可解释缺口及统计，不能用“收到过”替代。
- 精确故障和真实 Heartbeat/发现链路已执行。
- churn 内存/线程增长报告，不被退出释放掩盖。
- 风险、SKIP、负对照与正式通过率分开。
- 现有缺陷保留失败例和证据；修复另行决定，不悄悄改变契约。

## 7. 实施顺序与记录

1. 阅读本任务与目标路径规范，检查 git 状态，核对基线和已有测试资产。
2. 实现控制面、校验器、结果格式、watchdog、资源清理；证明校验器能识别确定性错误。
3. 补功能矩阵和非图/图/MultiTransport/fetch 生命周期。
4. 加真实 Heartbeat/发现、共段并发及过期链路。
5. 加仅测试构建注入，验证套圈、精确崩溃、重建、futex、churn、资源故障。
6. 运行独立 release 性能、sanitizer 和长跑，按结果更新记录与缺陷清单。
7. 按仓库流程提交代码和记录；记录完成日期、分支、代码提交。合并回开发分支、推送及参考文档同步依用户授权与仓库流程执行。测试完成且推送后更新 tm_kit_reference 相关章节并关联本任务。

每次确定/放弃方案、发现根因、跑完验证立即更新 README；中断后从本任务恢复。同一功能后续工作追加此记录。
