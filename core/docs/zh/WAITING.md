# 等待与 RPC 诊断接口

[English](../en/WAITING.md) | [简体中文](../zh/WAITING.md)

## 事件的使用约定

`include/wait_event.h` 在两个 libc 域之间只共享一个自然对齐的 32 位原子事件字，不共享 pthread 同步对象。默认实现通过 release 推进 sequence，再广播 `FUTEX_WAKE_PRIVATE`；等待方使用 acquire 采样并调用 `FUTEX_WAIT_PRIVATE`。

调用方必须按以下顺序循环：先采样事件，再检查或处理真正的业务条件，条件仍不满足时以此前采样值等待。返回后重新检查条件。通知发生在检查与 WAIT 之间时，内核比较返回 EAGAIN；EINTR 和伪唤醒也回到条件循环。事件字不是业务结果，不能仅凭一次唤醒判断请求已经完成。

等待与通知保留调用方 errno。非 EAGAIN/EINTR 的 futex 错误和必要的时钟错误属于运行时故障，当前实现终止进程。该事件只用于同进程共享地址空间，不能跨独立进程使用 PRIVATE futex。

空闲服务线程立即阻塞；只有在途 RPC 使用有限主动等待。`GW_RPC_SPIN_NS` 是单调时钟时间窗口，默认 20000ns，可设 0；它不是固定循环次数，也不保证在该时间内完成调用。退出时必须发布停止条件并广播，再确认相关线程退出后重用上下文。

## 构建配置

| 配置 | 默认 | 约束 |
| --- | --- | --- |
| `GW_RPC_PROFILE` | 0 | 0/1；启用统计和采样时钟 |
| `GW_RPC_SPIN_NS` | 20000 | 0..1000000ns；工作区构建入口检查范围 |
| `GW_FUTEX_WAKE_FILTER` | 0 | 实验事件编码，见 [通知过滤](WAKE_FILTER.md) |
| `GW_RPC_READY_POLL` | 0 | 实验响应代数轮询，依然保留事件检查以处理回调 |

所有共享协议参与者必须使用同一组配置。工作区组合器在每个项目 `build/<ABI>/generated/abi-flags.txt` 保存构建参数。不能单独替换 Host 或 Guest 后假设布局和通知协议兼容。

## 可选 RPC 统计

`rpc_metrics.h` 提供计数、scope 和快照，`rpc_handoff.h` 提供成对请求的分段采样。关闭 profile 的普通构建没有统计 TLS、诊断行与相应采样时钟；开启后的性能和内存成本必须单独测量。

scope 统计支持嵌套，区分包含子调用的时间与排除子调用的自身时间。采样 CPU 是当前线程 CPU，墙钟包含抢占和等待。实时快照不是事务视图；不能将不同采集时刻的计数强行相加，或将采样均值乘入口数称为精确全量 CPU。

通知零返回只表示此次 WAKE 没有唤起等待线程，不证明该通知可以删除。实验过滤开启时，NOTIFY 统计包含所有通知，但 zero/positive outcome 只包含实际执行的 WAKE。

## 配对交接诊断

同一次选中 RPC 保存发布前、被调用方接受、执行完成、调用方观察响应四个单调时间点；同一 context 的重入深度使用独立槽。约 1/64 的请求被选中。

| 分段 | 定义 |
| --- | --- |
| outbound | 发布前到接受，包括发布、通知、事件泵和调度 |
| execute | 接受到完成，包括内部计算、嵌套 JNI/RPC 和抢占 |
| resume | 完成到调用方观察，包括发布、通知、事件泵和调度 |
| transport | outbound + resume，不是纯内存传输时间 |
| execute_cpu | execute 区间内被调用线程的 CPU 时间 |

每个有效样本满足 `total = outbound + execute + resume`。慢样本汇总和有限的 outlier 记录只描述被选中的请求，不能据此估算所有长尾的发生率。`墙钟 − CPU` 也不能直接命名为调度延迟；需要同期调度 trace 区分睡眠与已就绪等待 CPU。

## 测试

从 core 目录运行 `ctest --test-dir build --output-on-failure`，或直接运行 `python3 tests/wait_events.py`、`python3 tests/rpc_handoff.py`、`python3 tests/ready_poll.py`。需要带 ASan/UBSan 的 Clang；输出在本项目 `build/test-work/`。

等待矩阵覆盖 32 种配置及受控通知竞态；交接测试覆盖 profile 开/关、嵌套、并发快照和时钟故障；响应轮询覆盖开/关 × profile 开/关 × 0/20000ns 共 8 种配置。`wait_events.py --ready-poll 1` 可再执行完整等待矩阵。桌面测试不替代目标 ARM64 的长期负载验证。
