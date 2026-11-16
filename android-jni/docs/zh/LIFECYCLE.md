# Runtime 生命周期与进程处置

[English](../en/LIFECYCLE.md) | [简体中文](../zh/LIFECYCLE.md)

## 支持的承载模型

每进程支持一套 glibc-worker JNI Runtime，绑定一个明确选择的目标 JavaVM。多个模组、ClassLoader 和 JNI 线程共用它。Android 应用入口的 ART 与嵌入的 HotSpot 是不同 VM；选中 HotSpot 后，不能把 ART 的 JNIEnv 传入桥。JNIEnv 仍属于各自的附着线程。

单实例是当前受支持的配置，不宣称多个 GNU loader/libc 必然无法共存。模块卸载不销毁 Runtime；同进程并发多 Runtime、关闭后重建、隔离后恢复均不支持。core 不依赖 JavaVM，也不负责选择目标 VM。

## 状态与入口

`full_host_state()` 返回任意线程可读取的原子诊断快照；它不持有资源，也不能作为随后调用必然成功的依据。

| 状态 | 含义与允许操作 |
| --- | --- |
| NEW | 注册可选 VM adapter，然后由所有者调用 start |
| STARTING | 一次性启动已被领取；不能并发启动另一套 |
| STARTED | Host 初始化成功；所有者启动匹配的 Guest，等待其 RPC 循环就绪，再调用 ready |
| READY | 允许正常装载与 JNI；所有者协调工作停止后可以调用 shutdown |
| STOPPING | 正在停止装载后端、恢复 VM 钩子、唤醒并 join Host 辅助线程及释放已确认安全的资源 |
| STOPPED | 终态；保留共享映射和单例标记，拒绝再次 start、ready、shutdown |
| QUARANTINED | 终态；拒绝新 Guest 操作及关闭，保留仍可能被使用的资源，由宿主处置进程 |

start 的配置检查或缺少 adapter 在领取启动前失败，状态仍为 NEW。领取后的任意初始化失败、Guest 握手失败、VM 钩子恢复失败或关闭中的 join 失败均隔离；不尝试把部分初始化的状态交给普通 shutdown。原有 pending exception 不因生命周期标记而被覆盖。

adapter 注册与生命周期入口由宿主串行管理。ready/shutdown 必须在成功 start 的线程调用；错误线程、重复调用或错误状态返回 JNI_ERR，不借用其他线程的 JNIEnv。start 的原子领取只保证一个初始化者，不让其它操作与初始化并发变得合法。

## 正常关闭顺序

1. 宿主先关闭业务入口，禁止新增装载、native 调用及线程附着；停止产生新任务。
2. 允许在途调用及其重入 JNI 回调完成，继续服务其请求。停止并 join 模组创建的后台线程；Host 绑定线程完成工作并进入宿主保证不会再发请求的空闲状态，Guest 附着线程 detach 并完成回收。Host 线程可以退出，但不要求 JVM 内部的空闲线程先退出。
3. 在执行 scope 外关闭所有模块；宿主同步缓存的 JNI/native 入口，不能只依赖借用计数推断后台工作已经结束。
4. 由启动线程在非重入状态调用 full_host_shutdown。允许 FREE 或已协调静止、无在途请求的 Host 来源 RUNNING 上下文；Guest 来源的附着及线程交接必须完成回收。所有普通/应急请求均须收到响应，RPC active 必须为零；模块、加载事务或关闭事务尚存时拒绝。单纯 JOINED 不代表 Host TLS 析构已完成。
5. 成功后仍由宿主要求 GNU 服务主线程退出并确认退出，再按自己的 VM 生命周期调用 DestroyJavaVM。关闭桥不等于销毁 VM、GNU libc 或 GNU loader 映射。

shutdown 是同步接口，没有内置期限。BUSY/未静止上下文的拒绝保留 READY，宿主完成协调后可重试。它不自动 unload 模组、不取消 native、不保证 pthread join 或第三方析构能在固定时间结束。宿主停止入口是前置条件；上下文检查是保守检查，不是与所有生产者并发竞争的 admission barrier。

## 停滞、取消与超时

当前不提供任意 native/JNI 调用的通用取消协议。同步调用发布后，参数可能仍指向调用方栈、JNI frame、共享缓冲区或 Guest 地址。watchdog 超期只说明在指定时间内未完成，不能证明这些地址不再被使用。

| 阶段 | 宿主可采取的措施 |
| --- | --- |
| 尚未进入桥的业务任务 | 在业务队列取消，未借出桥资源 |
| 已发布但尚未确认结束的调用 | 继续保留调用栈、引用、buffer、scope 和映像；不得伪造 response 或强制返回超时 |
| 模组有自己的协作取消接口 | 宿主可请求取消；必须获得“以后不再访问借用资源”的确认，或等待原调用真实完成，才能回收 |
| 超过业务允许期限，无法确认安全 | 从独立监督路径标记 gw_dlopen_quarantine；阻止后续入口，要求宿主丢弃执行进程 |

隔离不是取消确认，不解除任意 futex、用户锁、I/O、JNI 回调或 join。已发布调用可以一直等待，也可能收到真实迟到响应后正常展开；两种情况下隔离都不能撤销，不能据此重建 Runtime。已确认安全并完成的清理不回滚；隔离也不承诺停止正在执行的关闭代码。

不得通过 pthread_cancel、杀单个 Guest 线程、longjmp、修改响应序号或提前销毁 VM 来假装安全回收。仅移动共享帧到堆上也不能延长其间接引用的栈和 JNI 对象寿命。

## 宿主监督与故障边界

期限、诊断记录和是否丢弃进程由宿主决定；项目不设置适用于所有模组的固定超时，不自动杀应用。监督不能依赖已阻塞调用的返回；若要求在进程整体停滞后仍可处置，监督者应放在另一个进程。

可使用“启动器/监督进程 → 游戏执行进程”，后者同时承载 bionic、JRE 和 glibc-worker。JNI 仍在执行进程内直接桥接，控制通道只处理启动、状态与退出。这是可选的宿主部署方式，不是本项目新增的 Android service 或 JNI RPC 实现。

Runtime 和模组不构成内存或崩溃隔离边界。共享地址空间中的内存破坏、致命信号、exit 和进程级资源冲突仍可能影响整个执行进程。

## 验证

`python3 android-jni/tests/lifecycle.py` 用生产 Host 和 VM 替身验证九组独立进程用例：并发启动竞争、正常终止、映射失败、JNI 版本失败、adapter 安装失败、controller 创建失败、join 失败、adapter 恢复失败、握手失败；包括错误线程、重复关闭、重入/在途请求/未回收交接拒绝、空闲 Host 绑定允许和同进程重启拒绝。使用 ASan/UBSan，不执行 GNU Guest。

`python3 android-jni/tests/audit.py` 的在途用例调用生产 fh_command：隔离并唤醒后仍保留借用栈和帧；真实迟到响应才允许返回；响应永久丢失时由测试父进程结束一次性子进程。VM/Guest 为替身，父进程的测试期限不属于生产协议。装载借用等待及隔离后的 CLOSING 保留由 bionic-dlopen 的 close_waits / loader_quarantine 覆盖。

Android smoke/coverage 在真实 JRE、双 libc 环境输出 RUNTIME_LIFECYCLE_TEST，检查第二次 start、错误线程 shutdown、关闭终态、重复关闭与重建拒绝，之后仍须验证 Guest 主线程退出和 VM 销毁。破坏性停滞测试留在独立桌面子进程；不把正常真机测试描述为任意模组可取消或 ART 完整兼容。
