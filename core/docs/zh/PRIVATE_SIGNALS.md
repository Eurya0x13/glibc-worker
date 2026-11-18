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

启动期拦截将 Bionic 私有范围纳入 Host 路由，并在路由器启用前原生转发
`main` 之前的注册。真实 `android_run_on_all_threads` 注册、投递路径在
收养载体执行 GNU 代码时运行，原生 Bionic 处理器使用恢复后的 Host TLS。
它们对普通 Guest 系统调用的中断保留原生 EINTR 语义。

Android 后加载接入还必须在 [信号 profile](../../../jvm-jni/docs/zh/ADAPTER.md#signal-registration-integration)
中提供经审阅的所属 libc 制品和两个 Bionic action 导入位置。这些数据
位置在存在 ART 时保留已审阅的前端。已安装 JNI 测试在选定 VM 已运行时
执行 Bionic 注册，随后核对内核路由仍然完整。

## GNU Host 生命周期

同线程引导期间，路由器核对内核中的 GNU Host `SIGSETXID` 处置由 Host libc
提供，保留其标志与 restorer，并以原有 Host TCB 执行处理器。即使 Host
pthread 正在执行 Guest 代码，凭据广播也能得到该线程的确认。运行期间可校验
路由，关闭时精确恢复内核处置。每套 libc 的凭据广播覆盖各自登记的 pthread。
修改两个线程注册表所涵盖的进程凭据超出运行时契约。

信号 32 由原有 GNU Host libc 处理。载体安装 Guest TLS 时屏蔽该信号。
可能被取消的 GNU 原生调用者需要在桥接作用域内禁用 Host 取消，并在退出作用域后
恢复原先状态。原生回归验证了真实 GNU Host pthread 的取消和 `SIGEV_THREAD`
计时器，以及两个载体执行 Guest 时对原有 GID 的广播。回归在载体禁用取消时
提交取消请求，并在其退出桥接后检查清理函数使用了原有 Host TCB。恢复内核
处置后再次运行原生操作。原生强制展开跨越挂起的 JNI 或 VM 帧超出契约。

私有入口定义见
[GNU 取消实现](https://github.com/bminor/glibc/blob/glibc-2.42/nptl/pthread_cancel.c)
和 [GNU 凭据广播实现](https://github.com/bminor/glibc/blob/glibc-2.42/nptl/nptl_setxid.c)。
