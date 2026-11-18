# 已安装 Runtime 消费者测试

[English](../en/SDK_TESTS.md) | [简体中文](../zh/SDK_TESTS.md)

四条 `guest-signal-unwind` 观测还要求真实 GNU 线程在硬件故障处理器内取消或退出，
覆盖普通栈、替代信号栈；被中断函数与处理器都必须完成编译器清理，随后 JNI 调用
和退役也须正常。源码使用 DWARF 清理属性，并为故障读指令启用异常支持，避免
C 取消跳转的后备路径掩盖遗漏清理。归因于桥之前，可以独立运行同源 GNU 对照：

```sh
python3 android-jni/tests/guest_signal_unwind.py --runtime "$GW_RUNTIME"
python3 android-jni/tests/guest_signal_unwind.py --abi arm64-v8a \
  --runtime "$GW_RUNTIME" --serial "$ANDROID_SERIAL"
```

第二条需要 GNU ARM64 编译器及已授权的 shell 设备。两种对照均不加载 JVM 或桥。
结果、编译命令、运行时/二进制哈希和原始输出位于本项目
`build/tests/guest-signal-unwind` 或 `build/tests/guest-signal-unwind-android`。

用例消费分别安装的 core、bionic-dlopen 和 android-jni 包，嵌入显式选择的 HotSpot 制品，装载 glibc JNI 模块并检查调用、引用、关闭和退出，不以 ART 代替 Android JRE。

`python3 android-jni/tests/runtime_sdk.py --help` 列出桌面输入；Android 入口为 `python3 android-jni/tests/runtime_sdk_android.py --help`，还需显式设备、NDK、JRE 资源/库目录及匹配的 VM 启动 profile。部署前检查 libjvm/libjava 散列。Android 经 linker64 在 shell 进程执行，不是安装 APK；通过不证明应用沙箱兼容。

`--ffi` 选择 GNU Guest libffi（`core/build/ffi-linux/<ABI>`），Android 测试也如此；
`ffi-android` 下的 bionic Host 归档库不能作为 GNU 服务输入。

默认/worker 正向用例先拒绝两份错误桥配置，确认进程所有权尚未认领、状态仍为
`GW_HOST_NEW`、掩码及分发线程所有权不变，再成功启动真实 runtime。另有两个
`borrowed-relay` 用例为两个后端分别提供已运行的 Host 分发线程。关闭必须保留借用的
分发线程，并 join 自建线程；外部所有者随后在 VM/路由器清理前停止其分发线程。
所有正向用例（包括原生恢复）均要求 `runtime-relay-lifecycle` 观测。
拒绝不匹配 libc 则必须保留分发线程，并隔离已认领的桥。

Host 延迟掩码用例在创建 VM 前调用安装后的公开接口
`gw_host_signal_channel_enable`。core 的独立原生顺序、上下文和恢复失败用例负责覆盖
消费事务，详见 [Host 信道契约](../../../core/docs/zh/SIGNAL_WAITING.md)。

## 原生展开验证

使用 `-DGW_NATIVE_UNWIND_INTEGRATION=ON` 构建并安装 android-jni，然后给消费者 runner 传 `--native-unwind --nonlocal`，以链接 core 的显式拦截目标并构建匹配 Guest 服务。不传 --nonlocal 则检查普通同线程、worker 和缺少收养能力的普通 libc 拒绝路径。

定点发布/清理中断还需用 `-DGW_NATIVE_UNWIND_TEST_HOOKS=ON` 构建测试 SDK，
并给 runner 加 `--unwind-windows`。普通构建没有这些钩子；测试程序提供弱回调，
缺少回调不能通过窗口断言。测试 SDK 是验证制品，不作为部署替换件。

两项构建选项同时启用时，`--unwind-windows` 还要求十条 Guest 目标观测：执行两轮，
每轮分别覆盖保存/不保存掩码的一层跳转、三层跳转、300 个有效捕获的五层跳转，以及
强化入口的三层跳转。弱测试回调在 Host 返回缓存的 `GetVersion` 前重入 native 跳板，
不跨过 Java 帧。每条观测检查原 Host 掩码、再次 Guest 调用及无 JNI 异常。
最终关闭断言验证累计的活跃绑定和命令均已清理；缺少钩子或观测都会失败，
单独通过 Host 目标观测不能满足这组测试。

非局部用例执行 64 次 Host 目标信号跳转，一半位于保留外层 Guest 调用的 Host 回调内部，一半使用已安装备用信号栈。处理器先做局部跳转，再放弃内层 Guest 调用。断言覆盖掩码语义、退出信号栈、再次调用、JNI 异常状态、模块关闭、runtime 关闭和 JVM 销毁。信号 52 必须未被使用；Guest 原始 tgkill 按外部未标记信号契约刻意投递 Host，不占用 VM 保留信号。

另外的浮点用例设置不同的 Host/Guest 舍入模式，中断 Guest 后检查恢复及再次调用时
两侧模式正确。启用窗口测试后，再增加八次转移，分别在根层和嵌套层覆盖跳板准入、
命令发布、响应完成与跳板清理后四个位置：总计 74 次，其中 36 次嵌套、32 次使用备用栈。
Guest 入口计数区分尚未执行就撤销与已经完成的调用。这是定点覆盖，不是穷举中断验证。

用例还检查 JNI 回调、嵌套重入及后续调用期间不同的 Host/Guest 掩码；对已投递的 Host
信号检查 action 掩码、`SA_NODEFER`、正常返回和 Host 目标跳转。加 `--signal-pending`
可单独验收 Host 已屏蔽、Guest 未屏蔽的信号应保持待处理，并检查三个实时信号的 FIFO
顺序、原始发送者 PID/UID、载荷，以及 Host 解屏蔽前 `SA_RESETHAND` 动作不能提前耗尽。
匹配 runtime 将这些 Host 保留信号继续留在内核队列，不取出后重新构造发送者信息。
结果保存在独立的 `-pending` 目录。

匹配的原生展开服务还检查 Guest 同步 `SIGSEGV`、`SIGBUS`、`SIGILL`、`SIGTRAP` 及 seccomp `SIGSYS`
处理器、真实 Guest TLS 和 pthread 身份、保存/不保存跳转掩码、`setcontext`、修复缺页后
返回并重试指令、`SA_RESETHAND`、`SA_NODEFER` 嵌套故障、处理器内部局部跳转，以及
修改信号帧掩码。Guest 故障不得消耗 Host 的默认、忽略或一次性动作。八个 GNU 自建 pthread
并发执行 Guest `siglongjmp` 或 `setcontext` 恢复，不构造虚假的 Host TCB。
JSON 观测必须包含 15 项 Guest 检查及八个 Guest 故障线程。
SYS 用例只在可丢弃的载体线程安装不可撤销的过滤器，以 `getpid` 的特定参数触发故障，
普通调用继续可用，并检查 `siginfo_t` 中的系统调用号、架构及调用地址。

额外的 `guest-fpe` 观测启用 `FE_DIVBYZERO` 后执行真实浮点除法，检查 `FPE_FLTDIV`、
Guest 跳转恢复、后续普通 JNI 调用，并恢复原浮点环境。桌面要求
`PASS_REAL_HARDWARE`。AArch64 的异常陷阱使能属于可选硬件能力；原生
`feenableexcept` 无法启用时，Android 记录 `UNSUPPORTED_HARDWARE` 和
`recovered:false`，不计为 FPE 恢复成功。非预期信号、缺少观测或恢复失败仍使套件失败。

另外九项 Guest 注册检查覆盖 `signal`、`bsd_signal`、`ssignal`、`sysv_signal`、
`__sysv_signal`、`__sigaction`、`siginterrupt`、`sigignore` 和 `sigset`。八项执行真实陷阱，
检查 Guest TLS、errno 和处理器掩码，再经 Guest `siglongjmp` 恢复；忽略用例只查询动作，
不执行被忽略的同步故障。全过程须保留 Host 的一次性陷阱处理器；判定要求逐项 JSON
观测、再次 JNI 调用和已有关闭检查均通过。

另外五个独立进程检查 Guest 默认终止、Guest 一次性动作耗尽后的终止、GNU pthread
上的默认终止、被忽略的同步故障及 Guest 已屏蔽的同步故障。后两项遵循原生 Linux 强制故障
行为，不得无限重试或进入逻辑上已屏蔽的处理器；独立 core 原生 libc 对照也检查忽略真实
陷阱后的终止。判定同时要求 `SIGTRAP` 终止和故障前观测中的正确线程身份；任意非零退出码，
包括 runtime 的保守失败退出码，都不能通过。死亡用例不宣称正常关闭 runtime；正向用例仍须
完成模块关闭及 VM 销毁。

原生函数经真实桥跳板调用；目标检查点与被放弃的内层 Guest 调用间没有 Java 帧。
Guest 故障只恢复到同一个 Guest 调用内的检查点，不放弃暂停中的 Host 回调。
十九项信号栈用例覆盖独立 Guest 配置、启用/关闭 `SA_ONSTACK`、保存/不保存跳转掩码、
原生 `setcontext`、嵌套投递，以及 AUTODISARM 处理器通过 `swapcontext` 暂停，执行普通
Host 回调和第二次故障后恢复。GNU pthread 也执行普通、嵌套和 AUTODISARM 用例。
其中八项从 Guest 处理器进入 Host 处理器，再放弃 Guest 调用；这八次转移以 `stack_unwinds`
单独报告，不计入原有 74 次转移。断言检查 Host 普通/备用栈选择及 AUTODISARM 禁用状态的
保留，用例另行显式释放自身被放弃的 Guest 资源。

Host 信号 50/51 在屏蔽时须保持待处理，解屏蔽后投递 Host。生产路径不使用独立收养测试的
合成通知处理器，也不在载体退出时排空这些 Host 信号。独立 core 生命周期用例检查清理保留
应用的掩码修改和待处理信号，不将它们与 VM detach 行为混淆。

另有十九项等待检查覆盖 `sigpending`、`sigtimedwait`、`sigwaitinfo`、`sigwait`、`sigsuspend`、
`ppoll`、`pselect`、两种 epoll 掩码等待及 `signalfd`。每项收养用例都必须保留 Host 已排队的
三个实时负载、发送者信息及 FIFO 顺序。超时、Host 中断、错误路径及 NULL 掩码须恢复原掩码。
原生 GNU pthread 等待和取消作为正对照；两个 runner 均部署并记录取消所需 GNU `libgcc_s`
的散列。若环境对 `epoll_pwait2` 返回 `ENOSYS`，`guest_wait_skipped` 明确报告两项 epoll 跳过；
零表示未跳过。这个返回值本身不能区分内核缺少实现与系统调用策略限制。
另外两次 `wait_unwinds` 在发送跳出信号前，先通过自有载体的 `/proc/self/task/<tid>/syscall`
观察实际 `rt_sigsuspend`。它们检查保存/不保存 Host 掩码的跳转、再次 JNI 调用及用例显式清理
发送线程，不计入原有 74 次转移及八次信号栈跳转。判定必须有逐项等待/跳出观测，不能只靠汇总计数。
范围见 [信号等待](../../../core/docs/zh/SIGNAL_WAITING.md)。

通用 continuation 生命周期记账仍未完成。标记的 Guest 异步信号、
已测范围外的掩码/等待接口、剩余生命周期窗口及完整架构状态仍未证明，
属于 [恢复契约](../../../core/docs/zh/NONLOCAL_RECOVERY.md) 的要求。
通过原生转移跳过 Java/VM 帧不属于该契约；正常 JNI 返回、Java 异常传播及保留的
Java 帧仍须正确。

## Host 屏蔽同步故障的验收

在 `--native-unwind --nonlocal` 基础上加 `--host-fault-mask`，要求暂停中的 Host 屏蔽
`SIGTRAP` 时，Guest 仍可处理陷阱并恢复。结果另写 `-host-mask` 目录。这是要求成功恢复的
验收，不是预期死亡用例：被 `SIGTRAP` 终止必须判失败，不生成通过汇总。

本用例在创建 VM 前显式启用内部延迟故障信道，保留六个未使用的 Host 实时信号。
Guest 执行时在物理上解除故障信号的屏蔽，并屏蔽私有通知，Host 逻辑掩码保持不变。
发给已屏蔽 Host 故障信号的异步到达，在接收者队列中保留原始 `siginfo_t`，
通知只负责电平唤醒。真实 Guest 陷阱须进入 Guest 处理器，并通过自身 libc 恢复。

用例要求十三次 Guest 故障恢复。每次调用中的两次异步发送须合并，保留第一次发送的载荷、
PID、UID 和来源码。Host `sigpending` 显示原始信号，隐藏私有通知。第一组在 Host
解屏蔽后才调用处理器，随后三组分别由 `sigwaitinfo`、`sigtimedwait`、`sigwait` 消费，
再次零超时等待须返回 `EAGAIN`。有待决工作时释放上下文须返回 `EBUSY`，保持运行时
可用；消费后重试释放及再次 JNI 调用须成功。真正的线程退出只丢弃自身待决工作，
另由 core 用例验证。再有两次调用通过 Host signalfd 消费，分别使用普通读取、向量
读取和复制描述符。两者均检查原始元数据、消费前后的 poll 就绪以及再次 JNI 调用。
判定器要求存在正确的 `host-signalfd` JSON 记录。

Host `fdopen/fread` 另通过真实原生 FILE 消费 Guest 产生的信号包。
`host-signalfd-stdio` 记录要求原始元数据、fileno 身份保持、原生重开/关闭及随后
JNI 调用正常。core 的 [stdio 原生对照](../../../core/docs/zh/STDIO.md) 覆盖宽字符
和部分读取，并区分 GNU 隐藏读取修复与 bionic 原本已工作的路径。
另四次调用验证 read/readv 放弃及恢复后的新信号消费。原生 Host 处理器在内核消费后、
解码前跳出，不执行 JNI，也不跨过 Java 帧。每次恢复后均须允许释放上下文、再次 JNI
调用及新信号读取。判定器还要求存在正确的 `host-signalfd-unwind` 记录。
另外三次 Guest 调用验证设置 `SIG_IGN` 并恢复旧 Host 动作后，旧待决工作被丢弃，
上下文可释放，之后新信号读取及再次 JNI 调用正常。Host 忽略该编号期间，Guest 陷阱
仍须正常恢复，Host 异步到达则丢弃。判定器要求 `host-ignore-pending` 记录。

这个显式启用用例已在原生 Linux x86_64 和 Android ARM64/JRE17 通过，普通接入不会
自动启用。描述符生命周期/别名的接入缺口、并发忽略或重置语义及通用独立 Guest 异步投递
仍须验收。本用例和普通正向/死亡矩阵不能证明
其余要求已完成，见 [信号等待](../../../core/docs/zh/SIGNAL_WAITING.md)。

每次 nonlocal 回归还会从 Guest 向未屏蔽的 Host 动作异步发送 `SIGSEGV`，须保留发送者
元数据并以 Host TLS 执行，不能误入 Guest 同步故障路径。用例在 VM 创建前安装兜底动作，
由 HotSpot 持有外层动作，并通过原生信号链处理未识别的数据包；测试检查 VM 动作未改变。
JVM 信号检查告警仍会使本次运行失败。

另一个用例在载体持有 Guest TLS 时发送未标记的 `SIGUSR1`。Host 处理器须更新发起线程的
Host TLS 并保留发送者信息，返回后 Guest TLS 哨兵值和 errno 须保持正确；注册该 Host
处理器不得改变独占保留掩码。这验证收养载体上的外部 Host 投递，不代表标记 Guest 投递或
GNU pthread 的 Host 收养已经完成。

## 收养载体的独立 Guest 投递

用 `--native-unwind --adopted-signals` 运行已安装 SDK 测试。夹具指定一个未占用的
Guest 通知编号，并在启动时启用 Host 延迟信号通道。GNU pthread 将各消息发送到
收养载体，测试要求处理器收到原 TID、Guest TP、原生栈位置和负载。

用例覆盖 Java 忙循环、阻塞的 Host `ppoll` 和 `nanosleep`、Host 目标跳转、
穿过原生 `GetVersion` 的保存及不保存掩码的 Guest 目标跳转，以及 Guest/Host/Guest
嵌套投递。嵌套用例使用普通栈和替代栈，还验证跳向原 Guest 命令时退出两层信号调用。
每个用例在模组卸载和 SDK 关闭前再次调用 JNI。

Guest `siglongjmp` 和 `setcontext` 还可退出中间的 Host 回调及较新的 Guest 信号
调用，再恢复外层处理器。`setcontext` 用例从即将退出的栈传入上下文副本，检查目标
掩码与浮点舍入模式，并通过 `swapcontext` 往返信号调用前准备的独立 Guest 栈。
原生返回链接恢复仍属于当前信号调用的处理器。

跨作用域 `swapcontext` 用例让外层 Guest 处理器使用普通栈，内层使用
`SS_AUTODISARM` 备用栈。中间的 Host 回调暂停期间，两层 Guest 处理器往返三轮，
并执行三个额外的 Host 回调。断言覆盖两份 Guest 栈帧、原 Host 回调的栈数据、
各上下文的浮点舍入模式和掩码、备用栈重新启用、再次调用 JNI 和关闭。另一用例
验证 Guest 栈重叠时返回 `EINVAL`，内层处理器和中间的 Host 回调仍能正常返回。

正常返回的 Guest 处理器会修改暂停 GNU 上下文的浮点控制状态，下次原生入口必须
观测到这项修改。绝对时间 `clock_nanosleep` 用例要求，内核睡眠期间处理 Guest 通知
后，Host 仍按原截止时间醒来。

Linux x86_64 运行 18 个用例，其中 Host glibc 用例先恢复同作用域的 `swapcontext`
捕获，再恢复外层 `getcontext` 目标。Android ARM64 使用指定的 shell JRE17 运行
17 个用例。Guest glibc 上下文用例在两端运行。Host glibc 用例需要 Bionic 没有
提供的公开接口。

## 证据

输出目录包含 manifest.json 的命令和输入散列、原始日志及逐用例 JSON。超时、异常退出、格式错误的观测或 JNI/VM 处理器警告会生成 failure.json；只有全部选定用例通过才写 summary.json。中间 OBSERVED 行不是成功标记，运行开始删除旧汇总/失败文件。
清单还绑定 Guest 源码、已安装 core 头文件/栈切换汇编、libffi，以及消费者使用的 Host 归档库。

在工作区根目录执行 `python3 android-jni/tests/same_thread_results.py --require-current`，
可核验 21 组选定桌面/Android 测试的输入散列，输出
`android-jni/build/tests/same-thread-compatibility.json`。证据缺失、失败或过期均使检查失败。
只要 [todo.md](../../../todo.md) 的验收要求尚未完成，即便选定矩阵全部有效通过，完整兼容性
仍报告为 `INCOMPLETE`。

输出位于 android-jni/build/tests/，由 -android、-nonlocal、-recovery 后缀区分。它们是生成证据，不能修改清单使过期制品看似匹配。
