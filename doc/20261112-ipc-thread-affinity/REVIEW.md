# IPC Thread Affinity 代码评审报告

**记录日期：** 2026-11-13  
**相关任务：** [IPC Transport 内部线程通过 ConnectionLocator 绑核](README.md)

## 评审结论与验证范围

发现线程资源销毁顺序、构造异常处理、共享内存泄漏及 `driverCpu=none` 语义问题，共 4 项（P1 两项、P2 两项）。以下内容整理自本次评审输出；本次写入文档未重新运行测试，也未修复代码。

评审时，两项共享内存问题已通过定向测试验证。Singlecast 和 Aeron 的运行验证受沙箱禁止创建 socket 限制，相关结论来自源码检查。

## 评审意见

### [P1] 在销毁 Singlecast socket 前保留 io_context

位置：[src/singlecast/SinglecastComponent.cpp:409–413](../../src/singlecast/SinglecastComponent.cpp#L409)

当 provider 抛异常时，工作线程立即返回并销毁捕获的 `svc`；`th.join()` 返回后，`subscriptions_.erase()` 才销毁订阅，其析构中的 `sock_.close()` 会访问已经释放的 Asio service/reactor，造成 use-after-free，而不是安全地向调用者传播绑核异常。

建议：确保 `io_context` 的生命周期覆盖 socket 的关闭和销毁。

### [P1] 为线程启动后的构造异常补充停止和 join

位置：[src/shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp:270](../../src/shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp#L270)

共享内存空间不足时，后面的 `mem_.construct<SharedMemoryBroadcastClientListItem>()` 会抛出 `bad_alloc`。现在该分配发生在线程启动之后，构造函数展开会析构仍可 join 的 `th_`，直接触发 `std::terminate`，即使没有配置 `cpu` 也如此。

定向验证：将共享段剩余空间设为 400 字节，基线能捕获 `bad_alloc`，本分支则终止进程。

建议：在启动线程前完成可能失败的初始化，或保证后续异常会停止并 join 线程。

### [P2] 在绑核失败时回收已分配的共享内存对象

位置：[src/shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp:274–277](../../src/shared_memory_broadcast/SharedMemoryBroadcastComponent.cpp#L274)

当 provider 因核冲突或资源耗尽而抛异常时，这里只 join 后重新抛出，但初始化列表已经在共享段中分配了 `heads_` 和 `recordInSharedMem_`。构造失败不会执行订阅析构函数，`mem_` 的析构也不会销毁段内对象，因此每次重试都会永久泄漏这些分配；这些记录尚未进入客户端链表，也无法由发布者清理。

定向测试确认：连续失败每次减少 208 字节可用空间并增加一个命名对象。

建议：在异常路径显式回收这些分配。

### [P2] 根据解析结果跳过 driverCpu=none 的 provider 调用

位置：[src/aeron_ipc/AeronIpcComponent.cpp:305–308](../../src/aeron_ipc/AeronIpcComponent.cpp#L305)

配置 `driverCpu=none` 时，解析虽然成功，但结果被丢弃，随后仍强制要求 provider 并设置 `driverCpuConfigured_`。因此直接使用组件且未安装 provider 的调用会错误地抛异常；安装自定义 provider 后也会调用本应跳过的绑定逻辑，可能增加 `driverBindFailed`。

这与其他属性对 `CpuBindKind::None` 的处理不同，也不符合本任务规定的 none 不调用 provider 语义。

建议：根据解析后的 kind 跳过 provider 检查和调用。

## 处理结果（2026-11-13）

4 项意见均已修复，并各自补了回归测试；修复说明、验证命令和结果见 [README 的“评审修复”一节](README.md#评审修复2026-11-13commit-aed2a73)。

| 意见 | 状态 |
|---|---|
| [P1] Singlecast io_context 生命周期 | 已修复：订阅以 `shared_ptr` 持有 `io_context`，并保证它晚于 socket 销毁 |
| [P1] SMB 线程启动后的构造异常 | 已修复：段内分配全部移到启动线程之前，失败路径会 join 线程；旧代码在新测试下 abort，新代码通过 |
| [P2] SMB 绑核失败泄漏段内对象 | 已修复：`destroyUnlinkedObjects` 回收；新测试确认可用空间和命名对象数不变 |
| [P2] aeron `driverCpu=none` | 已修复：解析结果为 `None` 时跳过 provider |

回归中另外发现一个 SMB 既有崩溃（基线 `e22df3c` 同样复现）：读者无锁切换 tail，与发布者对链尾节点的 CAS 竞争，导致 use-after-free。修法是让读者切换 tail 时也持有 `listMutex`，详见 README。
