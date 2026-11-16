# 信号掩码与等待

[English](../en/SIGNAL_WAITING.md) | [简体中文](../zh/SIGNAL_WAITING.md)

本契约适用于显式启用 native-unwind 服务中的活跃收养 Guest 调用，必须使用匹配 libc。
它保护 Host 保留信号；完整 Guest 异步投递仍未完成。

## 接口行为

收养 Guest 拥有逻辑掩码，暂停的 Host 保留自己的掩码。
Host 保留信号的物理内核掩码采用 Host 位，因此 Guest 修改既不能解除 Host 的屏蔽，
也不能压制未屏蔽的 VM 信号。其余位遵循已有 Guest 掩码适配；原始系统调用绕过它。

| 接口 | 适配 |
| --- | --- |
| `sigsuspend`、`ppoll`、`pselect`、`epoll_pwait`、`epoll_pwait2` | 翻译临时执行掩码，保留暂停 Host 的保留信号位。 |
| `sigtimedwait`、`sigwaitinfo`、`sigwait` | 从可消费集合排除 Host 保留信号；后两者保留原生委托到 `sigtimedwait` 的路径。 |
| `sigpending` | 从 Guest 查询结果隐藏 Host 保留信号，不改变内核队列。 |
| `signalfd` | 创建或更新 Guest 描述符掩码时排除 Host 保留信号。 |

可消费集合不是执行掩码，将 Host 掩码位复制进去反而会授权消费 Host 信号。
只含保留编号的集合变为空集，定时等待保留原生超时/错误行为。
该机制不取出并重造信号：Host 实时信号的负载、发送者信息及 FIFO 顺序留在内核。

跨域传入的已有 Host signalfd 仍使用 Host 创建的掩码。适配器不接管任意描述符，
描述符共享规则仍由调用者定义。

## 掩码事务

`wait-mask.inc` 在屏蔽可捕获信号后保存物理/逻辑掩码，发布临时 Guest 逻辑掩码，
再构造物理系统调用参数，由所属 libc 的已有系统调用原子安装并等待。
正常及错误返回都恢复原有两种掩码，并保留返回值/errno。
如果只修改系统调用参数，Guest 处理器看到的逻辑掩码仍会是旧值。

TLS 中没有指向自动事务记录的指针，因此非局部跳出不会遗留指向该失效栈帧的指针。
所选跳转/上下文接口的掩码语义仍由原生保存/恢复适配决定。
Host 目标跳转通过桥展开协调，不执行被放弃等待包装的正常尾声。

NULL 临时掩码与未绑定 GNU pthread 保留原生路径，包括取消行为。
补丁保留 glibc 的取消包装和超时代码。已审阅路径为 x86_64/AArch64 上的 LP64 time64，
不宣称 time32 已验证。

## Host 延迟信道

链接 `glibc_worker::signal_interpose` 并初始化、接管路由后，在启动 VM、创建 Host
signalfd 或接收者前，调用 `signal_channel.h` 中的
`gw_host_signal_channel_enable(notifications, GW_HOST_FAULT_SIGNAL_COUNT)`。
集成者显式提供六个已审阅、未使用的公开 RT 编号；已有自定义注册或 signalfd 时返回
`EBUSY`，运行时不自动选编号，也不静默覆盖处理器。JNI SDK 的 `--host-fault-mask`
用例通过安装后的公开 API 接入匹配的原生恢复服务；普通 SDK 启动不会自动启用。

Guest 执行时需要解屏蔽故障编号，Host 仍保留自己的逻辑掩码。异步故障编号的原始
记录归属于一个载体接收者；普通信号合并并保留第一份 siginfo。私有通知不携带用户
指针，对六个编号进行公开注册返回 `EBUSY`。这不构成 Guest 独立异步投递。

Host 掩码查询、消费等待和描述符接口负责转换私有位。bionic 使用底层 LP64 入口避免
别名递归。Host 的 `sigsuspend`、`ppoll`、`pselect`、`epoll_pwait` 和 `epoll_pwait2`
保留原生原子掩码安装。汇编分别保存实际处理器入口掩码与等待返回掩码；Host 处理器
看到逻辑 `uc_sigmask`，有效修改在返回后保持。NULL 临时掩码保留原生行为。

## Linux 消费顺序与原子提交

Host 的 `sigtimedwait`、`sigwaitinfo`、`sigwait`、`read`、`readv` 和原生 FILE
读取后端使用相同的逻辑选择规则。符合条件的线程待决队列先于进程共享待决队列；
每个队列中，即使来源是异步包，Linux 也优先选择六个同步故障编号，再按最小编号
选择；其余信号按编号优先，RT 同编号保持 FIFO。已经分配到接收者的延迟包属于线程
待决；这不会倒退内核先前为进程定向信号选择接收者的决定。

延迟包参与选择时，候选集包含其最高优先级私有电平，以及逻辑优先级更高或相同的
原生编号。因此新到的高优先级原生信号仍由内核参与选择。同编号普通线程信号与较早
延迟记录合并，同编号进程共享记录则独立保留；出队后新发送的信号不会被当成重复包
排空。真正的原生 signalfd 提供原生记录布局，包括正故障码；用户态只编码自己保存的
延迟异步元数据。

内核选择、原始包消费与输出转换在短事务中完成，期间屏蔽可捕获信号并暂时禁用取消。
注册写锁将选择与 `SIG_IGN`、动作变更串行化，运行用户处理器前已经释放写锁。
`sigpending` 也在同一事务中取得一致的原生、逻辑快照。忽略代数丢弃较旧的待决工作，
后来恢复处理器也不会复活它；已经提交的输出仍有效。这约束消费选择与并发注册，
不代表下述异步处理器分派也已具有原生优先级。

每个延迟接收者拥有一个初始空掩码、非阻塞的原生 signalfd，与应用描述符分开，并在
接收者退休时关闭。短暂私有通知让内核校验和复制输出，保留原生 `EFAULT` 消费规则与
当前内核的向量布局规则。原始 iovec 元数据和单记录前缀均在逻辑出队前校验，零 iovec
仍返回原生的零。应用不得关闭、替换或导入运行时自有描述符。普通描述符及未启用信道
的调用保留原生路径。

所属 libc 的零长度读取、等待入口保留取消策略且不消费数据；GNU FILE 的不可取消
读取仍使用原生不可取消入口。真正阻塞发生在事务外的原子 `ppoll` 中，保留
`SA_RESTART`、`EINTR`、返回掩码、期限与 errno。等待记录使用稳定的线程自有存储，
不向 TLS 发布自动栈帧指针；信号在执行用户代码前摘除等待记录。因此嵌套读取、原生
`siglongjmp`、GNU `setcontext` 和保存栈的恢复不会留下悬空消费指针或半解码记录。
保存后被摘除的记录保留到正常恢复或线程退出，这不是通用 continuation 回收协议。
消费窗口不再依赖已初始化的跳转拦截器。

## 描述符与生命周期所有权

Host `signalfd`、bionic `signalfd64`、`read`、`__read_chk`、`readv`、
[原生 stdio 后端](STDIO.md)、复制别名及 `fcntl` 复制命令操作真实内核 OFD。
复制、导入的描述符共享原生掩码与状态标记，每次读取使用读取线程自己的接收者。
新内核允许记录跨 iovec，已测 Android 5.4 内核则在消费前以 `EINVAL` 拒绝该布局。
普通内核记录保持原始字节。

信道启用时，读取检查 `/proc/thread-self/fd` 与 `fdinfo`，覆盖私有 FD 表。没有数字 FD
缓存或需要退休的描述符租约。应用描述符所有者应串行化替换、模式变更与活跃读取。
proc 检查使用有界原始系统调用，在恢复投递前关闭临时描述符；不可检查时返回
`ENOTSUP` 且不消费数据。`poll`、`select`、`epoll_wait` 与掩码等待在检查就绪前只清理
已撤销的通知电平。绕过配置的启动、stdio 适配的原始调用或私有 libc 路径不属于此契约。

`close` 保留原生取消点，不持有元数据事务、写锁或隐藏副本，非局部退出不会退休复用
后的编号。启用、恢复时检查所有存活任务的真实表，包括私有 `files_struct`；保留私有
通知掩码的描述符阻止恢复路由，空掩码不再持有通知编号。

存活接收者有待决工作时，退休返回 `EBUSY`；线程退出只排空自己的通知并丢弃自己的
队列。所有者应等待接收者退出、销毁 VM、关闭或清空含通知位的描述符，并让保存的
continuation 静止，随后在启用线程恢复路由。恢复保留其他 Host 掩码变更。

恢复先快照原生动作，只写入需要改变的项，可恢复失败会回滚所有已完成写入；成功回滚
后路由保持活跃，允许重试。回滚失败、Guest 故障设置部分完成或无法恢复的 stdio 修改，
使后续操作返回终止性 `EIO`：保留代码与状态，拒绝准入和回收，由所有者丢弃进程。
运行时不会自动终止 Host。

## 验证与剩余边界

```sh
python3 core/tests/signal_deferred.py
python3 core/tests/signal_deferred.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

顺序用例将六种消费接口与原始 Linux 选择对照，覆盖混合优先级、线程及共享队列优先级、
普通信号合并、共享同编号记录与 RT FIFO。受控到达发生在候选集构造后；出队后新发送、
不可写输出与原生向量校验各有独立断言。阻塞 read/readv 先观察实际 `ppoll`，再检查
重启、EINTR、嵌套读取和跳向外部的原生跳转。GNU 专用用例覆盖上下文放弃、独立读取
后恢复保存的读取栈、阻塞中的 `setcontext` 及 close/context 转移。bionic 没有公开的
get/set/swapcontext 与 pthread 取消 API；明确报告不可用，不计入 Android 覆盖。

独立故障注入覆盖查询、首次写入失败，部分普通动作及辅助动作恢复的回滚，成功回滚后
重试，以及失败回滚必须保留路由且不能进入回收门。GNU 还拒绝 stdio 恢复中的可写
mprotect，检查回调回滚；bionic 已核实的原生 stdio 路径不需要修改表。

其他维护用例覆盖故障恢复、元数据布局、描述符导入与私有表、动作代数、一次性动作、
原生掩码、待决丢弃、消费取消与线程退出。内核、策略限制明确报告不可用。安装后的
JNI 验证还检查真实 Guest 调用、模块关闭、运行时停止与 JVM 销毁。Android 使用选定
JRE17 的 shell 进程，不据此宣称 APK 沙盒或 ART 兼容。

原生与延迟信号的**异步处理器分派优先级**、Guest 独立队列和标记发送、通用保存
continuation 生命周期，以及活跃 VM/ART 静止协议仍需验收。消费顺序测试不能证明
内核私有 RT 辅助通知的投递顺序等同原始信号的处理器顺序，显式启用时应保留这一区别。

上游实现：[Linux 信号选择](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)、
[原生 signalfd 复制](https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c)、
[原子 poll 掩码](https://github.com/torvalds/linux/blob/v6.12/fs/select.c)。
