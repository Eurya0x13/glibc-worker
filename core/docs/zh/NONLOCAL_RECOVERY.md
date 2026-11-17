# 跨域非局部恢复

[English](../en/NONLOCAL_RECOVERY.md) | [简体中文](../zh/NONLOCAL_RECOVERY.md)

本契约覆盖跨 libc 域的有效原生 continuation，包括桥持有的 JNI 资源，不覆盖通过原生
转移跳过 Java/VM 帧。正常 JNI 返回、Java 异常传播和保留的外层 Java 帧必须继续正确；
跨 libc 执行不能使这些受支持路径产生新的故障。

透明跨域恢复是同线程执行的首版要求。本文规定验收标准，不宣称能力已完整实现。显式启用的原生展开接入只覆盖部分契约，不能作为完整非局部兼容功能默认开放。

## 原生展开接入

`glibc_worker::nonlocal_interpose` 保持 Host libc 跳转缓冲区不透明，以尾调用转交原生保存入口。在创建接收线程或 VM 前，必须在串行启动阶段调用 `gw_nonlocal_init()`。Host 跳转沿被放弃的桥接 scope，在普通栈上逐层清理，期间屏蔽可捕获信号。匹配的 android-jni Guest 服务使用自身 libc 跳转入口退出被中断的 native 调用。JNI 清理在恢复的 Host 栈上执行，然后才完成原生目标恢复。这是放弃调用后的展开，不是恢复暂停的 continuation。

android-jni 的 `GW_NATIVE_UNWIND_INTEGRATION` 默认关闭。Host 库与 Guest 服务必须共同选择，私有传输能力握手拒绝不匹配制品；消费者显式链接 core 拦截目标。用例见 [已安装 SDK 测试](../../../android-jni/docs/zh/SDK_TESTS.md)。

用例覆盖 Guest native 执行期间恢复 Host 目标，包括保留外层 Java 回调、保存/不保存掩码、处理器内部跳转、备用栈、再次调用和关闭。它不跨过 Java 帧。独立 core 测试覆盖 glibc 强化入口 `__longjmp_chk`；最终恢复保留 libc 检查，不替换为无检查跳转。

启用带标签投递后，已安装服务可以先退出嵌套的 Guest 处理器，再穿过 `GetVersion`
原生回调跳向原 Guest 命令。[收养载体用例](../../../android-jni/docs/zh/SDK_TESTS.md#收养载体的独立-guest-投递)
也覆盖保存及不保存掩码的情况。

所属 Guest libc 在原生汇编的掩码操作处上报同步上下文的捕获和恢复。已记录的
`setcontext` 目标可以是原 native 命令，也可以是仍存活的外层 Guest 处理器。协调器
在清理前保留传入的上下文数据，等放弃的作用域释放资源后再调用所属 libc。Linux
Host 在尾调用 `getcontext` 前记录目标，并通过同一套 Host 作用域处理 `setcontext`。
原生恢复使用各 libc 的机器状态实现及目标逻辑掩码。

GNU Host 的 `swapcontext` 记录来源并尾调用原生 libc。Guest libc 还上报
`makecontext` 准备的新栈入口。提前构造的入口可以在当前作用域执行，返回链接可指向
本次 `swapcontext` 的来源或当前作用域内其他仍存活的上下文。Guest 若切换到已暂停的
更早桥接作用域，仍返回 `ENOTSUP`。恢复这些作用域需要同时维护两侧所属栈的生命周期。
Android 验证覆盖 Guest glibc 上下文和 Bionic 跳转缓冲区。

尚需完成 Guest 目标接入、可恢复原生上下文、状态发布窗口、完整架构状态与安全的
JNI 资源清理。Host 退出作用域后可复用其捕获槽。线程继续持有记录页及在桥接作用域
外捕获的记录。
未知检查点、失效 scope 代数及嵌套待处理转移当前会使接入失败；这些是待解决问题，
不是约定的首版语义。

core 收养测试另外验证受控的 Guest 目标恢复：Host 使用独立保护栈重新进入 Guest，
由 Guest libc 恢复自身仍有效的检查点。每个载体检查保存和不保存 `sigsetjmp` 掩码，
并对同一个 `getcontext` 快照调用两次 `setcontext`；随后还要完成普通调用和载体清理。
这个测试以协作方式保留 Host 帧，尚未实现 Guest 异步信号投递、放弃 JNI/VM 回调或
continuation 生命周期管理。使用匹配的收养 runtime 运行
`core/experiments/same-thread/run.py --adopt --signals`；Android 验证还需指定
`--abi arm64-v8a`、`--ndk` 和 `--serial`。

普通 native 调用接入在桥资源获取/释放、命令发布及 TP/栈切换期间屏蔽可捕获信号。
尚未执行的请求可由唯一载体安全撤销，已完成的请求不再进入 Guest abort。
正在执行的中断必须匹配确切命令，不能误用外层调用检查点。这些规则尚未覆盖所有
装载、生命周期及 VM 入口窗口。

从 Guest 进入 Host 处理器前，信号入口恢复暂停 Host 的浮点控制环境；
Guest abort 则从原始信号帧取得 Guest 的控制状态。此快照与协作切换目标独立，
因为信号可能恰好出现在 TP 已更换、目标浮点环境尚未加载时；改写切换目标会破坏正常返回。
ARM64 还保留被中断的 x18。这不等于完整 SVE/SME、PAC/SCS 或 x86 CET 兼容已验证。

显式启用的接入还将 Host 掩码保存在暂停的 Host 帧，将 Guest 掩码保存在其收养上下文。
Guest JNI 回调及嵌套 native 重入保留两侧掩码；从 Guest 进入的 Host 处理器使用保存的
Host 掩码，并结合已注册的 action 掩码和 `SA_NODEFER`。如果处理器通过已测试的 Host 目标
跳转放弃 Guest，Guest 掩码从被中断的内核帧恢复。收养后的活跃调用中，补丁版
`pthread_sigmask`/`sigprocmask` 对保留的异步信号维持暂停 Host 的物理掩码，并独立记录
Guest 逻辑掩码。所属 glibc 的 `getcontext`、`setcontext`、`swapcontext` 汇编使用同一适配，
保留原生寄存器、浮点及保护状态代码；未绑定的 GNU pthread 继续使用原生系统调用路径。
Host 屏蔽的保留信号仍留在内核队列，保留发送者信息、实时信号 FIFO 及延迟的一次性动作消耗。

[信号等待适配](SIGNAL_WAITING.md) 还翻译 `sigsuspend`、`ppoll`、`pselect` 及两种 epoll
掩码等待接口的临时掩码；消费型等待、待决查询及 Guest signalfd 创建/更新排除 Host 保留信号。
另有两次 Host 目标跳转先观察到真实内核 suspend 等待，再检查保存/不保存掩码、用例显式清理、
再次 JNI 调用和退出。这些机制未增加 Guest 异步待决队列。

初始化跳转拦截后，受管 Host signalfd 读取及消费等待还拥有独立展开 scope。信号在原生
消费之后、解码之前到达时，入口先将已消费原始信号转入该 scope；处理器新发送的信号
保持独立。跳向外部在普通栈上清理 scope，原生取消保留 libc 清理协议。已安装 JNI
用例检查 read/readv 放弃、上下文释放及再次 Guest 调用。这属于破坏性跳转恢复，
不代表通用 Host `setcontext`、暂停上下文恢复或 continuation 回收已经完成。

显式启用的服务拥有独立 Guest 同步故障动作。即使 Host 选择默认或忽略动作，core 仍为
六类故障信号保留路由入口；只有实际投递 Host 才消耗 Host 的一次性动作。
收养载体在进入 Guest 处理器前恢复 Guest TP、浮点状态及 x18；GNU 自建 pthread 保持
真实 Guest TP，不伪造 Host/VM 线程身份。正向用例覆盖真实 SEGV/BUS/ILL/TRAP 故障及 seccomp SYS 陷阱、
Guest 原生跳转/上下文恢复、嵌套处理器及后续 JNI 调用；独立进程以实际信号终止验证默认动作。

显式接入保留两侧独立的原生备用栈配置。屏蔽投递后，各域在 SP/TP 离开另一侧栈后安装自己的
配置，由内核在 Guest 选择的栈上构造 Guest 信号帧。从 Guest 转交的 Host 处理器按 Host
动作选择暂停的 Host 普通栈或备用栈。外层故障路由请求备用信号帧；逻辑动作未设置
`SA_ONSTACK` 时，处理器在被中断的普通栈执行，并保留 x86 red zone。
查询保留原生 `SS_ONSTACK`、`SS_DISABLE`、`SS_AUTODISARM` 行为。

自有栈用例覆盖正常返回、嵌套、保存/不保存掩码跳转、`setcontext`，以及使用 `swapcontext`
暂停 AUTODISARM 处理器后执行 Host 回调、处理第二次故障，再恢复原处理器；只有它结束后
才释放栈。另外八次 Host 目标跳转组合两侧栈设置，跨域放弃 Guest 处理器；用例显式清理自有
资源，检查 Guest 状态、后续调用和模块/runtime 退出。Guest abort 前保存 Host 的
AUTODISARM 状态，不会擅自重新启用跳出处理器后已禁用的 Host 栈。

这些尚不是完整独立信号虚拟化。通用保存 continuation 的生命周期及回收仍未完成。
标记的异步发送/待处理队列、GNU pthread 上的 Host 投递、已测范围外的
掩码/等待接口及切换窗口重入仍需完整信号设计。Guest 在单个 native 调用内恢复故障，
本身不证明可以放弃活跃 Host 回调并恢复 Guest 检查点，也不证明任意保存信号帧可重新恢复。

## 可观察行为

匹配的显式接入服务也在审阅过的纯 C `GetVersion` 回调边界协调 Guest 目标原生跳转。
glibc 钩子在原生寄存器保存之后观察捕获，在原生清理及掩码恢复之前观察跳转。
记录归属有效的 native 调用代数；调用退出时失效其记录，槽位复用，载体关闭时解除映射
达到高水位所分配的记录页。Host 捕获页回收和通用可恢复 continuation 仍需后续生命周期协议。

内层 Guest 跳转先退休被放弃的 Guest 命令，再由 Host 边界路由逐层退休相应命令、跳板
及 C 回调后缀。暂停的 Guest RPC 先正常完成协议和性能记录清理，再由所属 glibc 恢复
目标缓冲区。强化调用在原 Guest 栈通过 `__longjmp_chk` 完成，不以独立临时栈上的
未检查跳转替代。模组仍使用 `sigsetjmp`/`siglongjmp`，无需项目专用宏。

安装后的用例使用真实 JNIEnv 和桥接跳板，在 Host 返回缓存的 `GetVersion` 前注入测试
C 回调。覆盖保存/不保存掩码、一/三/五层嵌套 C 边界、300 个有效捕获、重复复用缓冲区、
强化源入口、后续调用及模块/runtime 正常关闭。具有临时所有权的其他原生 JNI 回调边界、
有效的 Guest 目标原生 continuation 及跨域 `setcontext` 仍属于必须完成的内容。
跳过 Java/VM 帧不属于本契约。

Host 检查点在载体执行 Guest 时仍可有效。Host 信号处理器可以跳回该检查点，放弃 Guest 调用；必须恢复一致的 Host continuation，并清理被放弃的桥工作。之后同一载体仍可调用，模块/runtime 可正常关闭。Guest 目标及嵌套场景有同等要求，不能因实现顺序将最终契约缩减为 Host 目标。

已有模组继续使用 libc 接口；要求改用项目专用 sigsetjmp/siglongjmp 宏会改变需求。每个缓冲区属于创建它的 libc 与逻辑线程；布局、指针保护及架构状态由该 libc 的已审阅适配处理，不能将 bionic 缓冲区交给 glibc，反之亦然。

目标必须仍是有效 continuation。跨载体跳转、已返回栈帧和语言级未定义行为不会因桥存在而合法化。跳转不回滚应用写入、不关闭任意应用资源，也不释放应用持有的锁。

## 恢复责任

| 状态 | 要求 |
| --- | --- |
| 执行 | 恢复目标栈、continuation、ABI 保留寄存器、TLS 及适用的架构保护状态。 |
| 信号 | 遵守所选 API 的掩码语义，保留逻辑待处理信号、嵌套及备用栈行为；未保存掩码的接口不能擅自恢复旧掩码。 |
| libc | 使用目标域 TCB 与缓冲区语义；不虚构原生 longjmp 未承诺的检查点时 errno 恢复。 |
| 桥接帧 | 精确协调被放弃后缀的命令深度、Guest 深度、rpc_top、活跃标记及请求/响应代数。 |
| 所有权 | 桥持有的模块借用、跳板活跃计数和临时记录恰好释放一次；保留外层调用与引用。 |
| JNI | 经安全 VM 路径协调桥持有的局部帧、引用令牌和在途资源；不能在信号处理器内任意执行 JNI 清理。 |
| 生命周期 | 防止之后恢复到已退休栈、TLS、信号入口代数或已卸载模块；记录仍可恢复的保存上下文。 |

setcontext 需要独立的 continuation 生命周期模型。恢复保存上下文可以继续暂停的工作，不能一律当成破坏性展开；信号帧上下文也不能与同步 getcontext 快照互换。每个选定 libc/架构都要明确并测试保存/恢复路径。

## 实现边界

core 负责 continuation 身份、域切换和信号帧生命周期；Guest runtime 补丁负责 glibc 保存/恢复接入。Host 必须覆盖实际 bionic/libc 路径，包括相关别名与强化入口，仅拦截一个公开符号不足。

android-jni 负责桥/JNI 资源协调；jvm-jni 负责受支持调用和生命周期所需的已审阅 VM 接入。
正常 JNI 返回与 Java 异常传播必须保留正确的 VM 状态。通过原生转移绕过 Java/VM 帧及其
清理不属于本契约；桥不实现 Java 栈展开，也不为此要求定制 JRE。
被放弃原生后缀之外的 Java 帧必须仍然可用。

在发布部分恢复状态前，必须定义信号重入与提交边界。需要分配、锁或 VM 入口的清理不能任意在信号处理器中运行。拦截必须保留原生调用约定，包括 returns-twice 和检查点函数的尾返回位置。

## 验收矩阵

[仅使用 VM 的原生转移对照](../../../jvm-jni/docs/zh/NONLOCAL_VM.md) 隔离了不属于本契约的
托管帧跳转。其观测用于说明边界，既不证明桥兼容性通过，也不阻塞原生恢复。
以下矩阵均不放弃 Java/VM 帧。

所有条目在可复现验证前都属于必须完成的要求，并非已通过清单。

| 场景 | 转移后的断言 |
| --- | --- |
| Host 信号中断 Guest 并跳回有效 Host 检查点 | 目标 TLS/栈正确；被放弃请求不可恢复；下一次调用成功。 |
| 跨 Host 回调恢复 Guest 目标 | Guest continuation 与所有权正确；保留外层帧可继续使用。 |
| 多层跨域嵌套 | 只按正确顺序协调被放弃后缀。 |
| sigsetjmp 保存/不保存掩码 | 遵守接口语义，嵌套和待处理投递正确。 |
| 普通、别名及强化跳转入口 | 真实模组路径均经过恢复记账，别名不能绕过。 |
| getcontext/setcontext 及支持的上下文切换 | 区分恢复与放弃，保留保存上下文生命周期。 |
| 嵌套处理器与备用栈 | 重复转移不遗留处理器状态，不回收仍活跃的信号帧。 |
| 桥状态发布前后的中断 | 不暴露部分提交的深度、响应或所有权。 |
| 活跃 JNI 引用与模块借用 | VM 检查通过；桥资源平衡；应用资源保持原生语义。 |
| 重复恢复、再次调用、关闭 | 无累积活跃标记、孤立请求、失效令牌、泄漏或关闭挂起。 |
| 无效/失效目标 | 行为确定且不破坏其他 continuation；不能替代有效跳转成功。 |
| x86_64 与 Android ARM64 | 使用实际匹配 libc；桌面通过不证明 Android 兼容。 |

信号注册回收的静止证明必须考虑 continuation 生命周期。处理器计数为零不能排除之后恢复到保存信号帧。

## 原生 GNU 线程展开

真实 GNU pthread 的硬件故障会尾跳转进入 `signal_guest_kernel_bind` 选择的
Guest 内核处理入口。外层汇编在跳转前撤销临时 Host 帧。Guest 取消和
`pthread_exit` 因此可以通过真实内核信号帧展开到被中断的 GNU 代码，不需要 GNU
装载器查找仅由 Bionic 装载的映像中的帧信息。Host 所有的异步信号仍走显式 Host
分派线程。未配置入口的 GNU 硬件故障继续失败关闭，不能交给 VM 解释。

所属 Guest 的 `domain_stack_call` 为保存原始栈
位置的寄存器提供 DWARF 规则，在 SP 指向另一栈时也成立。原生 GNU 展开器继续
使用自己的信号恢复入口识别：
[ARM64](https://github.com/gcc-mirror/gcc/blob/releases/gcc-14/libgcc/config/aarch64/linux-unwind.h)、
[x86_64](https://github.com/gcc-mirror/gcc/blob/releases/gcc-14/libgcc/config/i386/linux-unwind.h)。
这提供原生强制展开的帧遍历能力，不会为任意跨域 `longjmp` 增加 C++ 异常清理。

已安装 JNI 对照在普通栈、替代信号栈上测试处理器的原生 `read` 取消点和显式
`pthread_exit`。处理器、被中断函数的编译器清理记录都必须执行；随后 JNI 调用、
模块关闭和运行时关闭仍须成功。独立 GNU 对照使用同一源码，但不加载桥。两者
均采用 `-fexceptions -fnon-call-exceptions`，使故障读指令按
[GCC 代码生成契约](https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html#index-fnon-call-exceptions)
成为真实清理点。这些标志用于测试判据，不要求现有模组重新编译。对照不跨越
Java/VM 帧，也不声称收养载体支持取消或独立异步入口已完整实现。

## 源码依据

- [glibc 跳转恢复](https://github.com/bminor/glibc/blob/glibc-2.41/setjmp/longjmp.c)：libc 清理、可选掩码恢复与机器状态恢复。
- [glibc x86_64 寄存器恢复](https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/x86_64/__longjmp.S)：指针解码与条件启用的影子栈处理。
- [glibc x86_64 setcontext](https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/x86_64/setcontext.S)：明确说明同步上下文切换语义。
- [JNI 引用与异常所有权](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html)：VM 管理的局部引用记录及显式释放。

实际制品源码由运行时清单固定；链接解释机制，不允许替换为未经验证的 ABI。
