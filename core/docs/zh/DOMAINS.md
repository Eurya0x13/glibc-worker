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

维护的补丁片段位于 `core/src/runtime/`，构建配方及生成的源码与对象目录也归 core。
机制回归和 Android 应用夹具位于 [`core/tests/same-thread/`](../../tests/same-thread/README.md)。

收养版运行时使用 glibc 的 `mmap` 堆回退路径。Guest 装载器将自己的 libc 作为首个
实例启动时，仍与 Host 共用进程的 break。Guest libc 保留 `sbrk(0)` 查询，修改 break
的请求返回 `ENOMEM`。见 [glibc 对第二套 libc 的分配策略](https://github.com/bminor/glibc/blob/master/malloc/arena.c)。

所属 libc 的[逻辑掩码生命周期](MASK_LIFECYCLE.md)将 GNU 原生线程继承与
借用载体的物理掩码分开，并提供用于队列资格的可选观察器。它要求匹配的私有
能力值，不为 GNU 线程创建 Bionic 状态。

## 信号接入

原生 Host 在加载 VM 前使用[启动期信号路由](SIGNAL_BOOTSTRAP.md)。
`glibc_worker::signal_bootstrap` 为主程序提供全局信号注册拦截。

显式拦截目标通过项目专有入口保留静态库对象。只引用 libc 名称不足以保证装载：
检测工具运行时提供的弱实现可能在检查静态库之前就满足该引用。此目标提供强符号封装，
不负责组合任意第三方拦截器。

显式启动拦截目标还接管 `siginterrupt`、`sigignore` 和 `sigset`；GNU 构建另外接管
`bsd_signal`、`ssignal`、`sysv_signal`、`__sysv_signal` 和 `__sigaction`，防止辅助接口
经 libc 的内部注册调用替换内核路由入口。BSD 重启策略及动作掩码遵循载体 libc；
GNU 与 bionic 的 `signal()` 行为分别对照原生实现。一次性动作耗尽后仍保留原 flags 和 mask，
与原生查询一致。

Host 注册路由覆盖所有可捕获的公开信号，包括 `SIGUSR1`、`SIGALRM`、`SIGCHLD`
和未保留的实时信号。公开接口 `gw_host_signal_contract_init()` 返回包含所选
私有通道的所有权集合。GNU Host NPTL 保留原生实现。bionic 私有注册通过
[所属 libc 路由](PRIVATE_SIGNALS.md)接入，Guest NPTL 租用 63/64。启动期入口在
Guest 准入前接管已有的自定义 Host 动作。

对照用例逐项执行辅助接口，每次修改后都验证内核路由：

```sh
python3 core/tests/signal_aliases.py
python3 core/tests/signal_aliases.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

受管信号通过主程序导出的入口注册。显式 libc 句柄或裸系统调用会绕过该路由。

已约定的所有权规则：

1. 同步硬件故障归执行故障指令的域，优先于异步保留规则；Guest SIGSEGV 不得进入 VM 的隐式异常机制。
2. VM 保留的异步信号归 Host；运行时标记的 Guest 发送若命中保留信号，返回 EBUSY，不向 VM 发送。
3. 其他经 Guest 运行时显式标记的信号归 Guest。
4. 外部未标记信号归 Host，包括进程定向信号。

`signal_contract.h` 实现该分类。普通服务执行 Host 保留策略。原生展开构建增加独立的
Guest 故障处置、收养载体掩码和原生信号栈，涵盖已验证的 AUTODISARM 暂停与恢复。
掩码适配将 Host 保留的待决信号留在内核队列。故障路由仅在投递 Host 时消耗 Host 的
一次性处置。
GNU 自建 pthread 使用自己的 Guest TLS 恢复同步故障。

显式 V2 启动将 GNU libc 带标签发送和消费型等待接入 Guest 收件队列，支持线程定向队列、
共享队列及实时信号计数。同线程后端允许目标载体在执行 Host 代码或 Java 时进入 Guest
处理器。Guest signalfd 支持逻辑读取、就绪通知和描述符生命周期；已适配的 Host 等待
可以在内部通知后恢复。参见 [Guest 发送](GUEST_SIGNAL_SEND.md)、
[异步入口](GUEST_ASYNC_ENTRY.md) 和 [信号等待](SIGNAL_WAITING.md)。

GNU 自建 pthread 收到的 Host 信号交给专用的 Host pthread。原始信号帧保留至
分发线程返回。共享作业控制信号遵循 Linux 进程语义。参见
[分发契约](HOST_SIGNAL_RELAY.md) 和 [共享作业控制](SIGNAL_WAITING.md#共享-linux-作业控制)。

缺省启动选择同线程执行后端。独立 Guest 信号路径需要
[显式 V2 配置](../../../android-jni/docs/zh/RUNTIME.md)。

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

路由完整接管后发布的普通掩码注册也可以在线回收，包括预留回执信号之前的注册。
探针检查内核提供的被打断 PC。入口尚未绑定时重试。Linux 可以在第一条入口指令
执行前构造多层信号帧。每个嵌套回调拿到原生上下文之前，入口会沿这些帧保留对应
的未绑定旧代。接入方的完整屏障负责证明复制出去的 PC 已不可恢复。
[Linux x86 返回用户态](https://github.com/torvalds/linux/blob/v5.15/kernel/entry/common.c)、
[Linux ARM64 信号帧](https://github.com/torvalds/linux/blob/v5.15/arch/arm64/kernel/signal.c)。

普通路由保留公开信号掩码、内核重启与一次性处理器的标志，以及实际投递时生效的
临时掩码。libc 私有信号在入口绑定阶段暂时屏蔽，进入回调前恢复原有掩码。
绑定后的处理器和暂停栈使用本地动作快照。全屏蔽故障入口及纯数据记录沿用既有
证明。路由尚未完整接管原生信号链时发布的入口，以及缺少后来预留的 libc 私有信号
掩码的入口，仍由接入方通过 `signal_router_reclaim` 的完整屏障处理。线程清单证明
基于单一 Linux 线程组和普通线程创建语义，新线程从自己的入口开始执行。

已适配的 Host 等待将回执信号作为内部通知隐藏。适配之外的原生不可重启等待可能
收到 `EINTR`，由接入方选择可接受该行为的在线回收场景。完整屏障仍可用于 VM
停机及路由还原后。屏障提供者需阻止新接收者和注册，覆盖保存的执行上下文，并避免
等待回收器持有的写锁。独立契约测试覆盖 worker 全部 join、拒绝回收、后续投递，
以及 Host 的 `siglongjmp`。

原生跨域 `siglongjmp`、`setcontext` 和受支持的 `swapcontext` 路径遵循
[非局部恢复](NONLOCAL_RECOVERY.md) 中的上下文生命周期及清理契约。

相关内核路径是 get_signal/SA_RESETHAND 发布，以及 ARM64 的 setup_return/setup_rt_frame：

- [Linux 信号选择](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)
- [ARM64 信号帧构造](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c)

## 生命周期

clone 前的受检启动失败会清理临时映射并返回 errno。clone 后，即使服务握手失败，loader 和监督线程栈也保持固定；用户态完成标记不能证明内核线程已退出。隔离进程由宿主处置，core 不自动结束进程，也不在已保存 continuation 下卸载代码。
