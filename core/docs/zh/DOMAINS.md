# 同线程域

[English](../en/DOMAINS.md) | [简体中文](../zh/DOMAINS.md)

`glibc_worker::domains` 负责架构上下文切换、载体线程信号注册与 Host 信号路由，只链接 core、平台 C 库和 libdl，不查找 Java、JNI、libffi、ART 或 HotSpot。

`domain_context.h` 描述保存寄存器的 ABI。应用通常应使用上层运行时接口，不应自行构造这些记录。收养后的 Guest 使用受保护的独立栈，以及由匹配的补丁 libc 分配的真实 GNU TCB/DTV。跨域切换栈和线程指针，不共享 allocator 对象、FILE、pthread 对象或 TLS 地址。

## 运行时制品

先构建匹配的普通源码与依赖，再执行：

```sh
python3 core/scripts/build-domain-runtime.py --abi arm64-v8a --jobs 4
```

制品发布到 `core/build/domain-runtime/<ABI>/<key>/`。清单绑定 ABI、源码/补丁输入及私有收养 ABI。服务允许 JNI 调用前检查 `__gw_domain_capabilities`，缺少该入口或值不匹配的 libc 会被拒绝。这是能力协商，不表示任意 GNU 发行版都能未经验证嵌入。

维护的补丁片段位于 `core/src/runtime/`；构建配方及生成的源码/对象目录也归 core。`experiments/` 保留验证入口与历史证据，不再作为已安装域目标的实现来源。

收养版运行时使用 glibc 的 `mmap` 堆回退路径。Guest 装载器将自己的 libc 作为首个
实例启动时，仍与 Host 共用进程的 break。Guest libc 保留 `sbrk(0)` 查询，修改 break
的请求返回 `ENOMEM`。见 [glibc 对第二套 libc 的分配策略](https://github.com/bminor/glibc/blob/master/malloc/arena.c)。

所属 libc 的[逻辑掩码生命周期](MASK_LIFECYCLE.md)将 GNU 原生线程继承与
借用载体的物理掩码分开，并提供用于队列资格的可选观察器。它要求匹配的私有
能力值，不为 GNU 线程创建 Bionic 状态。

## 信号接入

普通域目标不导出 libc 的 sigaction、signal 或 sigaction64。早期启动可显式链接 `glibc_worker::signal_interpose`，在创建 VM 前初始化路由器；晚接入必须显式对接已审阅的信号链。固定 JVM/ART 制品 profile 与导入补丁归 jvm-jni。

显式拦截目标通过项目专有入口保留静态库对象。只引用 libc 名称不足以保证装载：
检测工具运行时提供的弱实现可能在检查静态库之前就满足该引用。此目标提供强符号封装，
不负责组合任意第三方拦截器。

显式启动拦截目标还接管 `siginterrupt`、`sigignore` 和 `sigset`；GNU 构建另外接管
`bsd_signal`、`ssignal`、`sysv_signal`、`__sysv_signal` 和 `__sigaction`，防止辅助接口
经 libc 的内部注册调用替换内核路由入口。BSD 重启策略及动作掩码遵循载体 libc；
GNU 与 bionic 的 `signal()` 行为分别对照原生实现。一次性动作耗尽后仍保留原 flags 和 mask，
与原生查询一致。

Host 注册路由覆盖所有可捕获的公开信号，包括 `SIGUSR1`、`SIGALRM`、`SIGCHLD` 和
未保留的实时信号。路由 Host 动作不会增加 VM 独占保留；`signal_router_reserved()`
只报告所选所有权 profile 及私有通知信号。GNU Host NPTL 保留原生实现；
Bionic 私有注册使用已审阅的[所属 libc 路由](PRIVATE_SIGNALS.md)，
Guest NPTL 独占租用 63/64。
已有自定义动作须在 Guest 准入前显式接管。`signal_router_guest_ready()` 对尚未接管的
自定义动作返回 `ENOTSUP`，不会修改它。启动阶段可在创建 VM 前初始化并接管；晚接入仍须
满足外层信号链已经审阅的要求。

对照用例逐项执行辅助接口，每次修改后都验证内核路由：

```sh
python3 core/tests/signal_aliases.py
python3 core/tests/signal_aliases.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

这覆盖可执行程序启动时的显式符号拦截，不会让晚接入 VM profile 自动处理未经审阅的导入，
也不保证拦截显式 libc 句柄调用和原始系统调用。

已约定的所有权规则：

1. 同步硬件故障归执行故障指令的域，优先于异步保留规则；Guest SIGSEGV 不得进入 VM 的隐式异常机制。
2. VM 保留的异步信号归 Host；运行时标记的 Guest 发送若命中保留信号，返回 EBUSY，不向 VM 发送。
3. 其他经 Guest 运行时显式标记的信号归 Guest。
4. 外部未标记信号归 Host，包括进程定向信号。

`signal_contract.h` 独立实现并测试分类，不等于实际投递已完成。普通服务仍执行 Host 保留策略；
显式原生展开服务另外安装独立 Guest 同步故障动作及收养载体掩码。其
`signal_router_guest_faults` 接入在 Host 默认/忽略动作时仍保留外层故障路由，仅在投递 Host
时消耗 Host 的一次性处理器。GNU 自建 pthread 可在真实 Guest TLS 下恢复同步故障。
Guest 执行期间，Host 保留的待处理异步信号继续留在内核队列。

这些是部分机制，不是完整双域独立投递。显式服务支持独立原生信号栈及已测的 AUTODISARM
暂停/恢复路径；[信号等待适配](SIGNAL_WAITING.md) 还在临时掩码等待、消费型等待、待决查询及
signalfd 创建/更新时保护 Host 保留队列。真正 GNU pthread 收到的 Host 信号采用
显式 [Host 分发线程策略](HOST_SIGNAL_RELAY.md)，改变处理线程并保留原始信号帧。
标记的异步发送和 Guest 端到端待决投递仍未完成。内部
[Guest 信号收件队列](GUEST_SIGNAL_INBOX.md) 已提供私有/共享队列选择、
跨队列实时配额以及通知/关闭事务，尚未接入异步 Guest TLS 入口或 libc
发送/等待钩子。最终要求见
[非局部恢复](NONLOCAL_RECOVERY.md) 与仓库 todo.md；普通 SDK 不会静默启用显式展开能力。

绕过运行时的原始信号系统调用不携带逻辑域身份。GNU 自建 pthread 也没有隐含的 Host TLS/JVM 线程身份，不能带着 Guest TP 执行 Host 处理器。

<a id="reclaiming-host-registrations"></a>

### 回收 Host 信号注册

每个内核路由入口绑定一代不可变的注册。Linux 可以在接收线程执行首条入口指令
之前选中该入口。VM safepoint 和活动处理器计数都无法覆盖这个窗口。

可选 API `gw_host_signal_reclaim_enable` 在路由建立后、启动转交线程或
Guest 辅助线程前，预留一个未使用的公开实时信号。正常 Host 控制代码调用
`gw_host_signal_reclaim`，超时单位为毫秒。这个信号和入口代码保留至进程退出。
接入方应保证原生投递可用，且应用信号、消费型等待和描述符均不使用该信号。

回收器在注册写锁内摘出已退役的一批旧代，随后允许各接收线程继续执行。
它将固定身份的 Linux 线程清单与内核线程计数比对，再逐个请求回执，包括 VM
内部线程和未附加到 VM 的 pthread。每个回执必须来自同一个已核实的线程身份。
收不到回执或线程清单发生变化时返回 `EBUSY`，保留整批旧代。下一次调用重新
核对线程清单。

在线证明覆盖内核入口屏蔽全部可捕获信号的旧代，包括经故障延迟通道发布的新
注册，以及从未发布入口的记录。这样的入口掩码能保证回执位于最后一次注册读取
之后。处理器和暂停的回调依赖栈上的动作快照继续执行。仍然发布的注册保持有效，
以其他掩码发布的旧代保留至接入方通过 `signal_router_reclaim` 提供完整屏障。
路由保留这些入口原有的投递掩码。线程清单证明基于单一 Linux 线程组及普通线程
创建语义，新线程从自己的入口开始执行。

已适配的 Host 等待将回执信号作为内部通知隐藏。适配之外的原生不可重启等待可能
收到 `EINTR`，由接入方选择可接受该行为的在线回收场景。完整屏障仍可用于 VM
停机及路由还原后。屏障提供者需阻止新接收者和注册，覆盖保存的执行上下文，并避免
等待回收器持有的写锁。独立契约测试覆盖 worker 全部 join、拒绝回收、后续投递，
以及 Host 的 `siglongjmp`。

首版要求透明恢复跨域 siglongjmp/setcontext；恢复责任和验收矩阵见 [非局部恢复](NONLOCAL_RECOVERY.md)。仅同域通过或隔离一个有效跨域跳转，都不满足该要求。

相关内核路径是 get_signal/SA_RESETHAND 发布，以及 ARM64 的 setup_return/setup_rt_frame：

- [Linux 信号选择](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)
- [ARM64 信号帧构造](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c)

## 生命周期

clone 前的受检启动失败会清理临时映射并返回 errno。clone 后，即使服务握手失败，loader 和监督线程栈也保持固定；用户态完成标记不能证明内核线程已退出。隔离进程由宿主处置，core 不自动结束进程，也不在已保存 continuation 下卸载代码。
