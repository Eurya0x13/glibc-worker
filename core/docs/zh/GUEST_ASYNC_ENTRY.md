# Guest 异步入口

[English](../en/GUEST_ASYNC_ENTRY.md) | [简体中文](../zh/GUEST_ASYNC_ENTRY.md)

core 提供供运行时通知使用、正常返回的 Guest 入口原语。它是独立 Guest 异步信号
的前提，不是完整 `sigaction`/`ucontext` 实现。已安装的 JNI 服务尚未启用此入口，
也尚未消费其独立 inbox。

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

回调可用有效的 Guest TCB、Guest FP/x18 状态和单独持有的保护页入口栈调用
`domain_guest_signal_stack_call`。它切换栈和线程指针，执行 Guest 编译的代码，
正常返回时恢复 Host 栈、TP/FP/x18。两个域都不调用另一套 libc 的分配器。接入方
必须在这些切换期间保持可捕获信号屏蔽。原语不会把 Host 内核帧伪造成 Guest
`ucontext`，也不会让未经协调的非局部退出变得安全。

## 等待恢复

执行隐藏工作前，接入方对实际被中断的 Host 内核帧调用
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

两个载体经过两批已 join 的生命周期，消费 28 个私有/共享 Guest 包，并检查
发送者、code、数值和队列来源。Guest 入口在没有 Guest 调用轮询时中断 Host
忙循环，在真正阻塞的
原生睡眠和 `ppoll` 期间执行，还中断由内核保存 PC 确认的 Guest 忙循环。每次
入口检查 TID、Guest TP/TLS、保护页栈位置、errno、FP 控制状态及 Guest 分配。
两种定时 Host 等待都透明完成；重试观测确认 150 ms Guest 工作从原始 500 ms
预算中扣除。每次实际 `ppoll` 还接收一个故意不带逻辑消息的通知，并且必须在
真正的 Guest 包发布前重新进入内核等待。发送准入关闭后，目标先排空通知、
完成 Guest 析构，再 join，随后才回收 TCB、栈和接收队列。关闭后的发布返回
`ESRCH`。

源码/制品哈希、构建/运行命令、设备身份和结构化结果保存在
`core/build/tests/guest-async{,-android}/`。摘要中的 `production_ready`、
`guest_public_signal_abi`、`jni_vm_validated` 和 `nonlocal_async_entry_validated`
保持 false。由测试持有的栈和发送者，只能建立这个正常返回原生夹具的静默边界，
不能证明 Java 执行期间的独立模组信号、保存帧恢复、真正 GNU 线程目标，或活跃
VM/ART 的回收提供者。
