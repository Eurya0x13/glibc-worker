# libc 私有信号通道

[English](../en/PRIVATE_SIGNALS.md) | [简体中文](../zh/PRIVATE_SIGNALS.md)

匹配的收养 libc ABI 7 将内核信号 63 用于 GNU 取消和 `SIGEV_THREAD`
计时器辅助线程，64 用于 NPTL 凭据协议；动态公开实时信号范围为 34 至 62。
模组必须使用所属 libc 的 `SIGRTMIN`/`SIGRTMAX`，不同 libc 中相同的偏移
不代表相同内核通道。硬编码 GNU 实现私有编号不属于模组 ABI。

JRE Host 还为网络中断协议保留内核信号 62，与 HotSpot 的 USR2 挂起/恢复
信号相互独立。参见
[libnet](https://github.com/openjdk/jdk17u/blob/master/src/java.base/linux/native/libnet/linux_close.c)
和 [libnio](https://github.com/openjdk/jdk17u/blob/master/src/java.base/unix/native/libnio/ch/NativeThread.c)。
Guest 向该 Host 保留通道的带标记发送返回 `EBUSY`。

上游 GNU NPTL 使用 32/33，与 Android 计时器、回溯通道重叠。若不隔离，
首个 GNU pthread 启动时就会替换进程级 Host 处理器，甚至早于模组注册。
参见 [GNU 定义](https://sourceware.org/git/?p=glibc.git;a=blob;f=sysdeps/unix/sysv/linux/internal-signals.h)
和 [Android 13 定义](https://android.googlesource.com/platform/bionic/+/android13-release/libc/platform/bionic/reserved_signals.h)。
构建器修改这些定义，保留 GNU 原生处理器、强制展开和计时器辅助线程，
并将补丁纳入构建配方哈希。

## 启动与生命周期

在串行启动阶段、启动匹配 GNU loader 之前调用
`signal_router_lease_guest_nptl()`。已安装 JNI runtime 的 owner 启动路径
执行这一步。两个通道必须仍是原始 `SIG_DFL`，没有路由注册或其他保留。
冲突返回 `EBUSY`，两个通道均不改变；调用要求已启用且健康的路由器。
成功后重复调用是幂等操作，不会创建额外 runtime。

租约在进程内永久有效。Host 公开处理器注册和查询返回 `EBUSY`，逻辑关闭
路由器后仍然如此。验证和恢复不改写 GNU 所有的处理器。GNU 根线程、
计时器辅助线程和映像固定至进程退出；逻辑桥关闭不代表原生线程已退出。
应用必须避免 Host 向这些通道发送信号，也不能绕过接口原始注册。
桥返回或销毁不会将通道交还 Host。

GNU 私有处理器直接在 GNU TLS 下进入，取消仍由所属展开实现处理。
借用载体仍拒绝 pthread 取消和凭据广播；更换编号不会将载体变成内核
创建的 GNU 线程。原生 GNU 私有处理器不调用另一套 libc 的函数。

在 Android 上，Host 保留掩码还包含实际私有范围 32 至 `SIGRTMIN - 1`，
使收养 Guest 掩码适配器能够保留载体 Host 掩码。Guest 公开处理器不能
替换这些 Host 处理器。保留私有掩码位不意味着
`gw_signal_contract_owner` 将其接受为公开逻辑信号包。

## 验证与限制

`signal_private` 检查冲突拒绝、租约无部分生效、原生 Guest 写入、验证、
恢复和永久 Host 拒绝。真实 [跨 libc 夹具](GUEST_ASYNC_ENTRY.md)
先记录内核 32..40 处理器，创建并取消具有清理处理器的真实 GNU pthread，
再触发 GNU `SIGEV_THREAD` 计时器回调。要求 Host 私有处理器不变、GNU
私有处理器已安装，清理与回调代码使用真实 GNU TLS。

隔离不等于完整 Bionic 私有处理器拦截。载体正在执行 Guest 时，Host
私有处理器仍必须经过 TLS 恢复入口；Bionic 延迟注册也需要经过审阅的
导入路由。这里不承诺拦截任意私有系统调用或支持未经适配的 libc 制品。
