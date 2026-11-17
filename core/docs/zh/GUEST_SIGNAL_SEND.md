# Guest libc 信号发送路由

[English](../en/GUEST_SIGNAL_SEND.md) | [简体中文](../zh/GUEST_SIGNAL_SEND.md)

匹配的收养 libc 提供可选、有版本号的启动期扩展
`__gw_domain_signal_send_hook`。它把公开发送接口接到集成方的独立 Guest
队列，继续使用 NPTL 自己的线程描述符和 `pthread_kill` 校验。
已安装的 JNI 服务尚未启用此接口。公开的 Guest 异步处理动作、掩码、等待和
处理器非局部退出仍需分别完成。

## 注册与回调

从所属 Guest libc 解析此扩展。私有 C ABI 为：

```c
typedef int (*sender_fn)(void *thread, int tid,
                         const siginfo_t *information, int *handled);
int __gw_domain_signal_send_hook(unsigned version, sender_fn sender);
```

版本 1 和非空回调可以注册一次。无效参数返回 `EINVAL`，重复注册返回
`EBUSY`。没有注销或替换接口：在运行时不再发送信号、也不再恢复任何保存的
延续之前，回调与集成状态都必须存活。注册属于启动阶段，必须先于应用线程
使用该服务。此增量扩展单独协商版本，不改变既有收养能力值或共享传输布局。
需要此功能的服务必须拒绝缺少该符号的旧 libc。

回调使用 Guest TLS 和 Guest `siginfo_t`。非空 `thread` 是借用的所属 libc
描述符，`tid` 是目标存活时的 TID。跨线程发送会在整个回调及原生回退期间持有
NPTL `exit_lock`；当前线程不需要退出锁。空描述符和零 TID 表示本进程的共享
Guest 待决队列。描述符指针或 TID 本身不能证明目标的独立接收者注册仍然存活。

已路由的请求设置 `*handled = 1`，返回正数错误码。设置为零则使用原生发送。
启用后的集成必须明确拒绝未知的逻辑 Guest 目标，不能意外转给 Host 处理器。
所属 libc 在回调之前物理屏蔽所有信号；获取注册表或队列锁以及返回时都须保持
该掩码。回调只能使用非阻塞通知，不能递归调用这些公开发送接口。libc 先恢复
内部 errno，再恢复发送者掩码，使未阻塞的自发信号能在发送 API 返回前执行。
真正处理器自身对 errno 的修改仍像原生实现一样可见；回调内部操作的 errno
修改则被隐藏。

队列已接受发布与通知失败是两种不同结果。接受了消息后不能返回“未发送”错误，
必须按集成策略重试通知或隔离接收者。夹具要求固定存活目标的通知必须成功。
独立 inbox 保留既有的准入、关闭、RT 配额和私有/共享选择规则。

## 接入的所属 libc 入口

| 接口 | 逻辑目标与消息 | 错误约定 |
| --- | --- | --- |
| `pthread_kill`、`raise`、`gsignal` | 已验证线程，`SI_TKILL` | pthread 返回错误码；raise/gsignal 返回 `-1` 并设置 errno |
| `pthread_sigqueue` | 已验证线程，`SI_QUEUE` 和原始 `sigval` | pthread 返回错误码 |
| `sigqueue(getpid(), ...)` | 共享 Guest 队列，`SI_QUEUE` 和原始 `sigval` | `-1` 和 errno |
| `kill(getpid(), ...)` | 共享 Guest 队列，`SI_USER` | `-1` 和 errno |

路由消息保留 Guest 发送者的 PID 和实际 UID。回调内部操作不会改变 pthread
调用方的 errno；成功的进程发送也保留 errno，真正处理器自身的修改除外。
线程信号零只探测接收者是否存活，
不入队。进程信号零、其他 PID、进程组、无效编号、NPTL 内部信号、`SIGKILL`
和 `SIGSTOP` 保留原生路径。abort 的内部 raise 路径也维持原生行为。
原始系统调用不会获得 Guest 归属标记。这些是明确边界，不表示完整的进程或
信号虚拟化。

内核线程退出后，尚未 join 的有效描述符继续遵循上游语义：现代
`pthread_kill` 返回零且不调用 hook，`pthread_sigqueue` 返回 `ESRCH`。
此扩展在启用路由的 `pthread_sigqueue` 路径中加强生命周期保护，在目标选择和
通知期间持有 `exit_lock`；未启用时保留上游实现。

## 验证证据

使用当前配方构建的运行时运行
[双 libc 夹具](GUEST_ASYNC_ENTRY.md#可复现的原生验证)。原有 28 条
消息改为经公开 `pthread_sigqueue`、`pthread_kill`、`sigqueue` 和 `kill` 发送，
接收端检查队列来源、code、发送者和队列值。每个收养载体另行检查六种公开
自发信号调用都在实际执行之后才返回（额外 24 条消息），并覆盖无效/内部编号、
保留信号和 errno。另一个不装载 hook 的所属 libc 进程对照六种接口中处理器
自身修改 errno 的行为；带标签夹具检查内部恢复不会抹掉这些真实处理器效果。

两组受控的真正 GNU 线程退出竞争把发送停在 hook 中，让目标到达 NPTL 退出
事务，验证目标 TID 仍然存活，再放行发送者。它们分别覆盖 `pthread_kill`
和 `pthread_sigqueue`、锁内原生回退，以及 join 前有效的已退出描述符探测。

夹具显式调度逻辑掩码，在改变指定共享接收者前确认消息已经消费。
[core 注册表](SIGNAL_TARGETS.md)执行真实查找、准入、选择和退休。它尚未接入
已安装的 JNI 服务，也不提供公开 Guest 处理器 `ucontext`。
这些发送入口验证不构成 JNI/VM 信号兼容性、
任意 GNU 线程目标入口或异步非局部恢复的验收。
