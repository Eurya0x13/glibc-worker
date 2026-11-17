# Guest 逻辑掩码生命周期

[English](../en/MASK_LIFECYCLE.md) | [简体中文](../zh/MASK_LIFECYCLE.md)

创建前的目标准入由独立的[线程创建生命周期](THREAD_LIFECYCLE.md)回调提供；
继承快照本身不提供该保证。

匹配的 libc 为所属运行时提供私有掩码绑定、继承与观测。这些接口编译在 GNU
libc 内，只能在其有效 TCB 下调用，并通过精确的私有能力值协商。它们不是
Bionic 线程状态适配器，也不是面向应用的 ABI。

## 绑定与继承

`__gw_domain_mask_bind(logical, physical, active, owned_bits)` 绑定由当前
线程持有的稳定指针。绑定活跃时，指定物理位使用 `physical` 的值，同时通过
`logical` 暴露其独立逻辑值；其他位保留原生行为。收养载体与真正 GNU pthread
均可绑定；GNU 线程仍持有真实 TCB，不需要 Bionic 状态。重复绑定返回
`EALREADY`。空指针或试图接管内核专用、GNU 私有取消/凭据位时返回 `EINVAL`。

NPTL 在 clone 前记录新线程的逻辑掩码。普通继承使用父线程的 Guest 视图，
包括父线程所借用载体的物理掩码不同的情况。显式 GNU 属性掩码优先。真正的
子线程最初在自己的内核线程上恢复该 Guest 掩码，不能继承父线程无关的
Bionic 载体视图。

子线程生命周期 hook 中的 `__gw_domain_mask_inherited(logical, physical)`
返回创建时的不可变快照，发生在子线程运行时绑定修改计划物理掩码之前。
它不查询当前掩码。初始 GNU root 和收养描述符没有此类原生子线程记录，返回
`ENOTSUP`，且不修改输出。子 hook 仍在可捕获信号屏蔽状态下执行，之后 NPTL
才恢复启动掩码并进入用户代码。在此绑定可为运行时私有通知准备不同的物理位，
由 NPTL 在原有解除屏蔽点应用；接收者设置必须先完成。这些快照本身不实现
尚未启动线程的接收者准入。

## 观测逻辑变化

在串行启动阶段通过 `__gw_domain_mask_hook(1, callback)` 安装唯一且不可变的
可选观察器。版本错误/空回调返回 `EINVAL`，替换返回 `EBUSY`。观察器在原生
解除屏蔽前、所有可捕获信号都物理屏蔽时接收新逻辑掩码，仅作用于活跃绑定。
普通掩码查询不通知观察器。

所属运行时可在此窗口更新目标资格并发出非阻塞通知。回调必须保留所需存储、
保持原生临界掩码，不调用应用代码，也不重入掩码操作。libc 保留回调前 errno。
通知失败属于运行时“已接受但未通知”的错误状态，不能把已提交逻辑变化伪装为
成功回滚。原生掩码提交失败时，在尝试恢复物理状态前重新发布旧逻辑值。

公开 `pthread_sigmask`/`sigprocmask`、原生上下文掩码操作及适配掩码等待的
临时 begin/end 共享该观察器。上下文寄存器恢复仍由所属 libc 汇编负责，
原生保存/不保存掩码的跳转语义继续区分。运行时直接修改逻辑掩码时，仍需自己
完成队列发布事务；观察器不拦截任意存储、原始 syscall 或自身信号处理器记账。

## 验证

`tests/guest-async/mask-control.c` 的匹配 libc 对照覆盖真正 GNU root 绑定、
未绑定和绑定子线程启动、默认/显式属性继承及 C11 创建，检查快照不变性、
物理通知位准备、查询行为、观察器临界掩码、errno、get/set/swapcontext、
保存/不保存掩码的跳转及临时/NULL `ppoll` 掩码。
`tests/guest_async.py` 使用显式选择的运行时，在 Linux 或 Android ARM64
构建并运行它。该对照不增加 JNI/VM 或完整独立 Guest 信号投递的验收结论。

双 libc 入口夹具还从真正收养的 Guest TCB 创建 GNU 原生子线程。父线程在
逻辑上屏蔽 `SIGUSR1`，同时让载体的物理位保持未屏蔽。普通子线程必须以
Guest 位已屏蔽的状态启动，显式空属性则必须未屏蔽。子线程不安装掩码绑定，
因此通过它实际的原生内核掩码证明继承，不只是检查虚拟查询值。

配方把 NPTL 描述符及创建路径修改纳入输入摘要。源码见
[线程初始化](https://github.com/bminor/glibc/blob/glibc-2.44/nptl/pthread_create.c)、
[描述符状态](https://github.com/bminor/glibc/blob/glibc-2.44/nptl/descr.h)
及[掩码操作](https://github.com/bminor/glibc/blob/glibc-2.44/nptl/pthread_sigmask.c)。
