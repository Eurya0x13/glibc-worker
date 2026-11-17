# Guest 异步入口

[English](../en/GUEST_ASYNC_ENTRY.md) | [简体中文](../zh/GUEST_ASYNC_ENTRY.md)

core 为收养载体上的 Guest 运行时通知提供入口和弃置恢复。原生夹具覆盖正常返回
以及跳向 Host 检查点的 `siglongjmp`。公开 Guest 信号处置、通用保存上下文和已安装
JNI 接入仍需分别验收。

## 入口和所有权

接入方通过内部启动期 API `signal_router_internal_action` 预留一个明确、未占用的
公开 RT 编号。路由使用内核选中的不可变注册代次，以及现有无 TLS 依赖的载体查找。
进入接入方回调前恢复 Host TP 和 ARM64 x18，并屏蔽所有可捕获信号。正常返回保留
Host errno。对该预留路由的公开注册、替换和查询返回 `EBUSY`；验证、回滚及恢复
沿用路由器原有事务。

先初始化并接管 Host 路由器，在预留路由前冻结接收者准入。回调和持有的数据必须
活得比所有已选中或已保存的帧更久。绕过本层的原始注册不能在之后安全接管。
该编号必须区别于 Host 预留信号、延迟故障通知通道和逻辑 Guest 信号。不能根据
相同的 `SIGRTMIN` 偏移推断两个 libc 使用同一编号：各 libc 独立报告公开信号下界。
原生夹具传递明确的内核编号，并检查实际 Host 注册，不假定某段编号可用。

[目标注册表](SIGNAL_TARGETS.md)在真实 TCB 绑定后管理准入、共享选择和入口 pin。
目标是已登记的 Host 载体，包括其正在执行 Guest 域的情况。此路由不是供真正 GNU
pthread 使用的 Host 回调入口；后者仍需独立的 Guest 原生生命周期和处置接入。

在有效的所属 Guest TCB 上用 `gw_guest_signal_execution_init` 初始化
`gw_signal_execution`。该源码与 `start.S` 由 Guest libc 编译。执行、替代信号投递
和弃置都使用带保护页的栈。执行与替代信号投递可共用映射，弃置栈必须独立。
调用记录和栈必须活得比所有活动帧及保存续体更久。

`signal_guest_execute` 在屏蔽可捕获信号时切入 Guest 检查点。离开 Host 栈后才
安装 Guest 物理替代信号栈。Host 来源入口传入实际内核帧，Guest 来源入口传 NULL
以保留外层 Host 锚点。嵌套 Host 处理器收到暂停的 Host PC、寄存器和逻辑掩码。
协作切换保存的 Host SP 限定处理器可用的栈空间。进入 Host 分派前恢复 Host
浮点控制状态和 ARM64 x18。

私有通知的内核帧放在正被中断的栈上。此前的路由帧仍可能占用 Host 替代信号栈，
而 Host 处理器此时运行在普通栈上。若私有通知额外设置 `SA_ONSTACK`，内核会
从已占用的栈顶重新写入。转发 Host 处理器时仍按应用处置自己的 `SA_ONSTACK` 选择栈。

跳向 Host 检查点的 `siglongjmp` 会从最内层开始，逐个退掉被跳过的原生作用域。
每层从单独的弃置栈进入 Guest，由所属 libc 跳回检查点。Guest 屏蔽信号后释放桥
持有的资源，切回一个新保存的 Host 恢复点。Host 恢复替代信号栈并解除锚点，再
继续前往目标检查点。活动入口会阻止回收，内核已选中的入口及保存续体仍由独立的
静止协议管理。

## 等待恢复

执行隐藏工作前，`signal_guest_execute` 对实际被中断的 Host 内核帧调用
`gw_signal_selection_hidden`，从而保留原等待的截止时间、原生余量及真正 Host
中断规则。Guest TLS 中执行的工作也计入同一份 Host 预算。空通知或过期通知同样
使用隐藏路径。选中的私有内核动作带有 `SA_RESTART`，但仅此不足以让睡眠/ppoll
透明恢复，因此仍需显式等待适配器。

此内部入口不提供私有位掩码虚拟化、Guest 处置/默认动作或消费等待。可选的
[所属 libc 发送扩展](GUEST_SIGNAL_SEND.md)现已在夹具中接入带标签的公开发送。
测试接入显式控制通知掩码和发送者。普通阻塞 I/O 以及私有/原始 Host
等待仍受[信号等待](SIGNAL_WAITING.md)中列出的边界约束。

## 可复现的原生验证

使用匹配的收养运行时，并明确指定经过审阅的通知编号：

```sh
python3 core/tests/guest_async.py --runtime "$DOMAIN_RUNTIME" --notification 55
python3 core/tests/guest_async.py --runtime "$DOMAIN_RUNTIME" --notification 55 --ubsan
python3 core/tests/guest_async.py --abi arm64-v8a \
  --runtime "$DOMAIN_RUNTIME_ARM64" --notification 55 \
  --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

脚本构建并安装 core SDK，再构建独立 GNU 可执行文件，通过现有动态链接器在
同一进程启动。准入前 Host/Guest 比较共享结构大小和偏移；Guest 检查精确的
私有 libc 能力值。队列操作由 Guest libc 编译和执行。Host 对相同逻辑编号的
原信号动作保持安装，并接收自己无标签的内核信号包。
可选的原生 `--ubsan` 运行使用 Clang 检查 Host/core；GNU Guest 及其 libc 不插桩。
该配置使用独立输出目录。

两个载体经过六批已 join 的生命周期，消费 84 个私有/共享 Guest 包，并检查
发送者、code、数值和队列来源。Guest 入口在没有 Guest 调用轮询时中断 Host
忙循环，在真正阻塞的
原生睡眠和 `ppoll` 期间执行，还中断由内核保存 PC 确认的 Guest 忙循环。每次
入口检查 TID、Guest TP/TLS、保护页栈位置、errno、FP 控制状态及 Guest 分配。
两种定时 Host 等待都透明完成；重试观测确认 150 ms Guest 工作从原始 500 ms
预算中扣除。每次实际 `ppoll` 还接收一个故意不带逻辑消息的通知，并且必须在
真正的 Guest 包发布前重新进入内核等待。发送准入关闭后，目标先排空通知、
完成 Guest 析构，再 join，随后才回收 TCB、栈和接收队列。关闭后的发布返回
`ESRCH`。

60 次嵌套 Host 投递检查原始 Host 上下文、掩码修改、TP/TLS、浮点控制和
普通/备用栈位置。六批组合覆盖 Host 备用栈禁用、启用和 AUTODISARM，以及
Host `SA_ONSTACK` 开关，包括处理器内原生栈查询的标志。每个载体还在
原生 `ppoll` 的隐藏 Guest 工作期间接收一个真正的 Host
信号，该等待必须返回 `EINTR`。独立的一次性对照故意不提供 Host 锚点，必须
观察到错误上下文，证明仅有 Guest 调用成功不足以发现此类失败。

12 次跳向 Host 的恢复各退出三层 Guest 入口。每层 Guest 清理按进入的逆序释放
自己的目标 pin。Host 验证掩码、TLS 及替代信号栈状态，包括 AUTODISARM 的
原生行为。后续通知、普通 Guest 调用及接收者关闭都必须成功。每个调用记录只用一次。

源码和制品哈希、构建运行命令、设备身份及结构化结果保存在
`core/build/tests/guest-async{,-android}/`。原生测试以
`host_jump_guest_retirement` 和 `retired_async_entries` 记录已覆盖的跳转路径。
`nonlocal_async_entry_validated` 保持 false，等待 Guest 目标及保存上下文恢复。
`production_ready`、`guest_public_signal_abi` 和 `jni_vm_validated` 同样为 false。
