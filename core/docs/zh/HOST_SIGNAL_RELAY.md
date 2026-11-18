# GNU 线程收到的 Host 信号

[English](../en/HOST_SIGNAL_RELAY.md) | [简体中文](../zh/HOST_SIGNAL_RELAY.md)

真正的 GNU pthread 拥有自己的 GNU TLS，没有挂起的 Host 调用栈。
执行 Guest 代码本身不需要 bionic 线程状态。但进程共享的 Host 信号处理器
可能被内核选中，在该线程上进入。显式转交策略让处理器在专用、真实的
Host pthread 上运行，不借用其他线程的 TLS，也不将 GNU 线程冒充为 JVM 线程。

## 投递契约

- Guest 同步硬件故障继续由原 GNU 线程上的 Guest 处理器执行，归属不变。
- 收到需要路由的 Host 异步信号时，将内核选定的不可变处理器版本、原始
  `siginfo_t` 和仍存活的内核 `ucontext` 交给 Host 分发线程。
  GNU 入口仅使用无锁原子操作和原始系统调用。
- 原接收线程在屏蔽可捕获信号的状态下等待 Host 处理器返回。因此请求和
  信号帧始终存活，无需逐信号分配、固定队列容量或复制含架构指针的 FP 状态。
- 处理器看到的 TLS、`pthread_self()` 和内核 TID 属于 Host 分发线程。
  第三个参数描述原 GNU 接收线程，受支持的上下文修改由其内核 sigreturn 应用。
  原始 `siginfo` 保留发送者、代码和载荷。
- 逻辑处理器掩码、`SA_NODEFER`、一次性处理器和不可变版本由现有路由器处理。
  原线程的系统调用重启仍由内核选定的 `SA_RESTART` 决定。
  分发线程初始没有备用信号栈，因此 `SA_ONSTACK` 按原生规则回落到普通线程栈。
- 队列中的请求依次处理；分发线程开放处理器掩码期间收到的原生 Host 信号
  仍可经正常路由嵌套进入。不同 Host 线程的处理器之间没有全局互斥。

这是明确改变处理线程的策略，不是透明的原线程投递。
处理器不能等待被阻塞的原接收线程推进。跳入另一 OS 线程栈的原生跳转无效，
转交不会使其合法。分发线程不会自动附着到 ART/HotSpot，也不会让处理器内的
JNI 或其他非异步信号安全操作变得合法。VM 挂起协议应针对真实的 VM 线程。

内核投递规则见
[`kernel/signal.c`](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)，
比较后阻塞的等待规则见
[`kernel/futex/waitwake.c`](https://github.com/torvalds/linux/blob/v6.12/kernel/futex/waitwake.c)。

## 接入和生命周期

安装的内部头文件 `signal-relay.h` 提供 `signal_host_relay_start()` 和
`signal_host_relay_stop()`。纯 C 宿主先初始化并收养路由器，再从 Host 生命周期
线程启动转交线程，然后允许 GNU 代码执行。JNI runtime 启动接口在启动 Guest
服务前完成这些步骤，两种执行后端采用相同规则。

停止前需要使外部发送者和 GNU 接收者静止。有请求持有原始信号帧时返回
`EBUSY`；分发线程自身停止时返回 `EDEADLK`。忙碌失败保留可继续工作的分发线程，
可以稍后重试。成功停止会 join 真实 Host pthread。JNI 关闭先检查未完成请求，
再关闭桥和 Guest 控制通道，最后停止自建的分发线程。JNI 启动时仅借用宿主已创建
的分发线程，其所有者在 runtime 关闭之后、路由器还原之前负责停止。
桥配置被拒绝且状态仍为 `GW_HOST_NEW` 时，不创建分发线程或启用 Guest 故障路由，
可以修正配置后重试。
从创建分发线程之前到 join 完成，转交线程一直占用路由器。包括启动和关闭在内，
整个区间内还原路由器都会返回 `EBUSY`。回收协议保留排队请求引用的不可变版本。
桥认领初始化之后的失败沿用保留资源并丢弃进程的既有契约。

## 验证

`signal_relay_test` 检查 Host TLS/errno、原始元数据、处理线程变化、32 个并发
注册接收者、选定版本被替换、忙碌停止以及线程创建和 join 窗口的还原保护。
这些是注册为 GNU 入口的
Host 单元测试线程，不冒充第二套 libc。

安装式 JNI 原生恢复夹具另在独立 GNU libc 中创建 8 个真实 GNU pthread，
发送 64 个线程定向 Host 包，包括异步 `SIGTRAP`。检查覆盖 Host TLS、GNU
TLS/errno/pthread 身份、原始元数据、原上下文掩码修改、随后 JNI 调用以及
runtime/VM 正常关闭。结果验证器强制检查 `gnu-host-relay` 结构化记录。
这不代表通用 Guest 标记异步投递或未经审阅的 ART 已兼容。
