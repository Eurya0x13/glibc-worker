# Guest 信号收件队列

[English](../en/GUEST_SIGNAL_INBOX.md) | [简体中文](../zh/GUEST_SIGNAL_INBOX.md)

内部 `signal-inbox.h` 队列是 Guest 独立异步信号的待决状态基础，编入
`glibc_worker::domains`；安装的内部源码也可由拥有它的 Guest libc 编译。
不得在 Guest 线程指针下调用 Host 编译的队列函数。跨域共享的是数据，
不是 libc 分配所有权或函数实现。

V2 JNI 服务通过此队列投递带标签的 Guest 信号。匹配的 GNU 发送钩子提供消息，
处置表选出处置，目标入口使用自己的 Guest 栈和 TLS。收养载体可以在运行 Host
或 Java 代码时处理消息。参见 [Guest 发送](GUEST_SIGNAL_SEND.md) 和
[异步入口](GUEST_ASYNC_ENTRY.md)。

## 选择与所有权

每个收件队列属于一个域，包含一个进程待决队列。每条稳定的接收者注册
拥有自己的线程待决队列和逻辑屏蔽字。Host 内核待决队列不参与 Guest 选择。

选择通过同一事务访问两个队列。线程队列优先于进程队列，即使进程包的
信号编号更小。每个队列内部先考虑六类故障编号，再选最小编号；以
`SI_QUEUE` 异步发送的故障编号也遵守这一优先级。这对应 Linux 的
[`next_signal`、`dequeue_signal` 和 `collect_signal`](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)。
普通信号在每个队列内分别合并并保留首个 `siginfo_t`；实时信号在相同编号、
相同队列内保持 FIFO。

`gw_signal_inbox_deliver` 在选择写锁内读取接收者屏蔽字，即使通知已经过期
也不会消费被屏蔽的消息。消费式等待使用 `gw_signal_inbox_take` 显式传入
可选集合，其中可以包含被屏蔽的信号；队列不会擅自将此参数与保存的屏蔽字
取交集。待决查询返回两类队列的并集，不受逻辑屏蔽影响。

发布 Host 保留编号返回 `EBUSY`；非法、不可捕获、glibc 私有编号 32/33
以及正数内核来源 `si_code` 返回 `EINVAL`。这个内部操作不实现信号 0 的
存活探测、公开接口的用户指针复制检查或进程组发送。

## 通知与生命周期

每项操作都要求调用者在物理上屏蔽可捕获信号，包括 libc 私有的取消信号。
需要使用原始屏蔽保护；Guest 虚拟屏蔽字，或过滤了私有编号的 libc 包装函数，
均不足以满足此条件。收件队列写锁串行化发送、屏蔽变更、
选择、配额和接收者关闭。通知回调只能发送非阻塞通知，不得重新调用收件
队列接口或直接执行应用处理器。其他接收线程可以立即进入通知处理器，等待
发布结束；自身通知则在调用者释放写锁后恢复屏蔽时才会进入。

回调收到定向接收者指针，进程待决消息则传入 `NULL`。接入层必须为共享
消息选择符合条件且存活的 Guest 接收者；所有接收者都屏蔽时保留消息，
并在接纳新接收者时检查共享待决状态。解除已有接收者的屏蔽会为符合条件的
私有或共享消息触发通知。仅注册元数据不会在接入层发布栈/TLS 状态前
就使新线程暴露于异步入口。

发布结果区分拒绝和唤醒失败：

| 结果 | 含义 |
| --- | --- |
| `status != 0` | 本次调用未排入消息。 |
| `status == 0`、`wake_error == 0` | 已接受或合并；通知成功或无需通知。 |
| `status == 0`、`wake_error != 0` | 消息已经接受；需要重试通知或终止性处理。 |

消息已被接受且允许重试时，只重试失败的通知。重试整次发送可能重复实时信号。
通知失败后，先前已合并的普通信号消息保留原位。

在同一写锁下关闭接收者后，不再接受发往它的消息或通知，并归还其待决实时
信号配额。关闭组会拒绝新消息并丢弃共享待决队列，各接收者随后关闭私有
队列。销毁是独立操作，要求发送者、处理器和保存的延续状态都已经静止。
存活接收者计数归零本身不能证明这个边界。保存的原生延续状态仍可能引用
元数据时，不得回收或复用。接入层的稳定注册负责目标生命周期，缓存的 TID
不能充当生命周期令牌。

配置的实时信号上限由域内所有线程队列和进程队列共用，是本地资源预算，
并不等价于 Linux 按 UID 统计的 `RLIMIT_SIGPENDING`。内核通知的分配及失败
仍单独处理。普通信号不占实时配额；队列块按需增长并保留到静止销毁，没有
固定消息表。

## 验证

运行原生 Linux 或 Android ARM64 测试：

```sh
python3 core/tests/signal_deferred.py
python3 core/tests/signal_deferred.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

`signal_inbox_test` 将十二种私有/共享队列场景与实际内核 `rt_sigtimedwait`
对比，覆盖屏蔽、合并、异步故障编号优先级、实时 FIFO、发送者元数据以及
线程先于进程的选择。并发生产者和消费者检查消息恰好消费一次；资源用例
覆盖跨接收者配额、拒绝、关闭和静止后复用。受控的并发关闭不能在通知事务
尚未结束时撤销目标生命周期。GNU 异步取消用例验证先完成发布，再恢复物理
屏蔽允许取消进入；Bionic 没有 `pthread_cancel` API。通知测试能在没有 Guest 调用或
队列轮询的情况下中断忙碌的原生 Host 线程，并验证屏蔽保留、失败唤醒重试
以及 Host 对同一逻辑编号的处理器不受影响。该测试只使用一套 libc，不证明
Guest 入口/TLS 或 VM 兼容性。

独立夹具将 Guest 编译的队列接到真实收养 TCB 和带保护页的入口栈上，覆盖
Linux 与 Android ARM64 原生 Host/Guest 代码的及时中断，以及 Host 睡眠/ppoll
恢复。已安装的 V2 用例还覆盖 Java 执行期间的 Guest 投递和原生恢复。参见
[跨 libc 入口夹具](GUEST_ASYNC_ENTRY.md) 和
[已安装 SDK 测试](../../../android-jni/docs/zh/SDK_TESTS.md)。

可选的纯原生复现 `signal_inbox_test --notification-control` 排入一个没有逻辑
消息的私有通知，再由 `ppoll` 临时屏蔽字原子地允许它进入，报告
`CONTROL_ONLY`：即使设置了 `SA_RESTART`，Host 等待仍在没有 Guest 消息时返回
`EINTR`。测试套件将其记录在 `native_controls` 中。已安装服务使用 Host 等待
适配器在 Guest 投递后保留截止时间，并向调用者报告 Host 信号中断。参见
[等待恢复](SIGNAL_WAITING.md) 和
[Linux poll 重启处理](https://github.com/torvalds/linux/blob/v6.12/fs/select.c)。
