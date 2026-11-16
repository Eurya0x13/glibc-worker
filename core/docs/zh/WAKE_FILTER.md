# 实验性通知过滤

[English](../en/WAKE_FILTER.md) | [简体中文](../zh/WAKE_FILTER.md)

`GW_FUTEX_WAKE_FILTER=0` 是默认通知协议。设为 `1` 可构建实验候选：同一个4字节事件字的最低位保存 sticky waiter hint，上31位保存 epoch。它没有增加共享区字段，但改变了事件编码，Host、Guest、嵌入入口必须整体匹配构建。

候选只在通知推进 epoch 前的字中有 hint 时调用 `FUTEX_WAKE_PRIVATE(INT_MAX)`。没有 hint 也推进 epoch，保留发布语义。等待者确认 epoch 未变后，以同一个原子字的 CAS 设置 hint，再使用完整的 epoch|hint 字调用 WAIT。任何等待返回都不清 hint；只有通知方以一次 CAS 同时推进 epoch 和清 hint。虚假/过期 hint 只导致额外 WAKE。

## 无丢唤醒的论证范围

设 W 为设置 hint 的成功 CAS，N 为同时推进 epoch 与清 hint 的成功 CAS。两者在同一个原子字上有修改顺序：

- N 先于 W：W 对旧字的 CAS 失败或随后的 epoch 检查返回，不会按旧值入睡。若已到内核 WAIT，完整字比较发现变化，返回 EAGAIN。
- W 先于 N：N 读取到 hint 并执行 WAKE。若线程已经入队，则广播唤醒；若广播先于真正入队，N 已改变事件字，WAIT 的比较会失败。
- 多等待者共享 sticky hint。等待者正常返回、EINTR、重入或离开时都不清它，所以一个等待者退出不会取消另一个等待者的标记。多通知方使用同一个 CAS 推进，不能拆成“先加 epoch，后清 hint”两个操作。

等待 CAS 使用 acq_rel/acquire，通知 CAS 使用 release/relaxed。该方案不依赖独立的两个原子字互相观察，因此没有把两字的普通 acquire/release 误当成防止 store buffering 的证明。原有 sample→pump/check→wait、predicate 循环、广播关闭和 context join 后重用顺序保留；不涉及线程指针或 libc TLS 切换。

Linux WAIT 对同一futex字进行原子比较并入队的接口依据见 [FUTEX_WAIT](https://man7.org/linux/man-pages/man2/FUTEX_WAIT.2const.html)，广播及返回值依据见 [FUTEX_WAKE](https://man7.org/linux/man-pages/man2/FUTEX_WAKE.2const.html)。上述用户态协议论证仍是本项目对该接口的使用推论，不能用测试通过代替完整证明。

该论证和原方案一样排除观察窗口中发生完整 epoch 回绕的 ABA。候选将 epoch 空间从32位减为31位；它没有解决无限期暂停/完整回绕问题。原始 sequence 数值不再是通知次数；sample 返回清除 hint 的偶数值，每次通知加2。标记、ready 与计数都不能代替真实内核阻塞证据。

## 启动匹配

候选 Host 在装载激活前的闲置 call[0] POD 参数7写 HELLO；候选 Guest 在开始服务时验证并主动向参数8写 ACK，随后现有 phase=2 release 发布。Host ready 自行 acquire 读取 phase 后才读取 ACK；成功值保存在 Host 私有原子状态中，避免后续命令复用 call[0] 后再次误读。原RPC诊断的参数0..6和操作字段不变。此检查在 profile=0 时也启用。

旧 Guest 不会主动产生该 ACK，候选 Guest 也不会接受缺少 HELLO 的旧 Host，因此拒绝激活。错配失败应在可整体结束的独立进程中处理，不能依赖候选过滤后的正常 shutdown 唤醒可能按旧协议入睡的 Guest。默认0路径维持原有启动逻辑；该模式检查不是通用的所有构建配置协商接口。

## 统计与测量

NOTIFY scope 仍包含每次通知，旧 wake_syscalls 只记录实际 WAKE 调用；ABI2 zero/positive outcome 也只记录实际 WAKE 返回。跳过 syscall 不能记成 zero_return，候选下分类之和可以小于 NOTIFY。真实性能对照使用 profile=0、spin=20000，同一fixture/Java payload，关闭采样、JIT符号导出和诊断，按 A/B/B/A 完成保存退出和数据库验证。桌面夹具可定量证明 syscall 减少，但不能直接换算成游戏收益。

测试 hook `GW_EVENT_TEST_HOOK` 仅在测试翻译单元显式提供时有效，普通构建为空。点1为已登记、点2为入WAIT前、点3为WAIT返回后、点4为通知CAS后。测试不复制协议实现。

## 使用状态

过滤默认关闭。该实现用于验证通知协议与 syscall 成本，不能仅凭 syscall 数减少就启用到产品。采用它需要 Host/Guest 匹配构建、完整竞态测试，以及关闭诊断的真实负载 A/B 数据。回退也必须整体替换两侧实现。
