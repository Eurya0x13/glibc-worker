# JNI 失败处理

本契约适用于有效附着线程经桥调用 JNI，及 Host 进入 Guest native 的路径。失败不会覆盖调用方已有的 Java pending exception。异常类查找或抛出本身失败时，保留 VM 留下的异常；桥不承诺在全系统 OOM 下仍能创建指定类型的异常对象。

## 错误映射

| 原因 | JNI 路径 | C 装载接口 |
| --- | --- | --- |
| 引用/ID、注册记录、跳板、线程上下文分配失败 | 保留原异常，否则 `OutOfMemoryError` | 有错误参数的入口返回 `GW_E_CAPACITY` |
| 上下文/调用帧扩容失败或索引达到溢出边界 | 保留原异常，否则 `OutOfMemoryError` | 命令记录 `GW_E_CAPACITY` |
| 描述符过长、非法参数、单次注册方法数不在 0..64 | `IllegalArgumentException` | `GW_E_ARGUMENT` |
| 失效/异类 token、模块已关闭、桥不可用 | `IllegalStateException`；已隔离的 native 入口维持 `UnsatisfiedLinkError` | 按原因返回 BRIDGE、NOT_READY、QUARANTINED 等状态 |
| libffi 准备失败（不是 closure 分配失败） | 注册抛 `UnsatisfiedLinkError` | `GW_E_BRIDGE` |

异常保留优先于表中的新异常类型。正常 JNI 实现返回的异常、NULL 和数值遵循真实 VM；桥不会将所有 NULL 都解释成资源耗尽。优化用数组缓存没有空间时回退原 JNI 实现，正常回退不抛异常。

发生桥失败时，具有 JNI 状态语义的函数返回 `JNI_ERR`，包括 Throw/ThrowNew、PushLocalFrame、EnsureLocalCapacity、RegisterNatives、UnregisterNatives、MonitorEnter/Exit 和 GetJavaVM。其他引用/数值返回值使用 NULL/零值，调用方必须按 JNI 规则检查异常，不能把数值零当作成功证明。Host trampoline 在失败退出前初始化返回槽；参数 token 化失败时不会调用 Guest，也不会把失败的引用当作合法 NULL 继续调用真实 VM。

Guest `AttachCurrentThread` / `AttachCurrentThreadAsDaemon` 失败始终清空输出 JNIEnv；上下文或 worker 内存不足返回 `JNI_ENOMEM`，VM 的其他附着错误保留原状态。尚未附着的线程没有可用 JNIEnv，因此不通过虚构 Java 异常传递失败。使用未附着/错误 JNIEnv 属于非法调用；RPC 拒绝并设置 errno，不能保证为该线程创建 pending exception。

## 动态帧与应急槽

普通 Host 命令和 Guest RPC 初始为 12 层，之后按 16 层增长，不以 12 为执行上限。独立应急 RPC 在扩容失败时报告错误，并允许异常查询/清除、DeleteLocal/Global/WeakGlobalRef、字符串/数组释放和 MonitorExit。应急处理期间 Host 不允许再发 Guest 命令，避免同一应急槽被递归占用。

ExceptionOccurred 若因 token 分配失败无法返回对象，已有异常仍保留；ExceptionCheck 和 ExceptionClear 不依赖分配引用槽。VM 或线程栈本身的 StackOverflowError 仍保留，但桥不承诺无限递归或在原生栈耗尽后恢复。

私有协议为 v6，使用稳定动态目录、独立应急 RPC、逐上下文诊断地址和 owner 生命周期待办。Host/Guest 必须一起重编部署；握手检查共享结构、帧布局、初始容量和页宽，不能混用 v5 或更早版本。完整线程与所有权契约见 [CONTEXTS_AND_OWNERSHIP](CONTEXTS_AND_OWNERSHIP.md)。

## 回滚与资源所有权

VM 已创建的 local/global/weak 引用若无法转换成 token，删除本次新引用；借用的 native 参数不能由存储层删除。ID 插入失败删除本次新建的 weak class。注册在到达 VM 前失败，撤销本次新建的跳板和未发布类记录，保留已有记录；到达 VM 后可能已经发布部分方法，资源保留到正常注销。TLS 绑定或 Guest worker 创建失败时撤回本次预留上下文。

不确定的注销、清理失败继续遵循 [永久隔离契约](LOADER_BACKEND.md#卸载与失败隔离)：保留可能仍可达的资源，由宿主丢弃进程。普通资源不足不自动触发永久隔离。显式注销有主类失败也属于不确定清理，保留其所有权记录及跳板。

## 验证范围

`tests/failures.py` 以 ASan/UBSan 编译生产 Host/Guest RPC，注入共享帧/上下文页、私有记录、引用/ID 扩容、TLS 绑定和 worker 附着失败，检查错误状态、原异常、回滚和后续调用；使用 VM 和 worker 替身，不执行 Android 双 libc。`tests/bindings.py` 使用真实 libffi closure 检查帧/参数失败时不发命令，以及注册预留/扩容和 closure 分配失败的资源平衡。`tests/resources.py` 检查存储与 token 边界，`tests/audit.py` 检查装卸载事务。

Android 组合 coverage 在真实 HotSpot 的严格 CheckJNI 下运行三轮 32 层重入和 Host/Guest 各 32 线程扩容、注册数量拒绝及释放后复用。结构化结果要求零桥错误和最终零活跃引用/closure；映射失败与应急 RPC 的故障注入在桌面生产路径夹具完成。它不等于全系统内存耗尽实验，不包含完整 ART 验证，也不保证任意模组能够恢复自身的部分操作。

## 生命周期失败

启动领取后的初始化失败、Guest 握手失败，以及关闭中的 VM 钩子恢复或 join 失败均使桥永久隔离。错误线程、错误状态和忙碌上下文的关闭请求直接拒绝，不进入资源释放。同步调用没有通用超时返回或取消确认协议；宿主的 deadline 不允许释放仍在借用的参数、引用或映像。完整顺序与监督责任见 [生命周期契约](LIFECYCLE.md)。
