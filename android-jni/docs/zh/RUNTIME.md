# Runtime 启动接口

[English](../en/RUNTIME.md) | [简体中文](../zh/RUNTIME.md)

安装的 `runtime_bridge.h` 提供 `gw_host_runtime_start` 与 `gw_host_runtime_shutdown`，使用已安装的 core 域目标和正常 JNI 桥，不依赖实验头文件或测试可执行程序。

启动配置带版本并要求显式输入：

- backend：零选择同线程，`GW_EXECUTION_WORKER` 选择 worker。
- loader_path、library_path、service_path：由宿主部署的匹配 GNU runtime 和服务。
- bridge：已有映像策略、JNI flags 和可选已注册 VM adapter。
- signals：与已准备 Host 信号路由器匹配的不可变所有权契约。

目标 VM 由传入的真实 JNIEnv 确定。android-jni 不创建 VM，也不选择 ART/HotSpot profile。宿主在允许应用调用前准备信号链；固定 VM 的晚接入由 jvm-jni 提供，不得传入另一个 VM 的 JNIEnv。

start 在桥初始化前检查 `signal_router_guest_ready()`。已有自定义 Host 动作未经显式接管时
返回 `ENOTSUP`。可执行程序启动阶段可在创建 VM 前初始化并接管路由器；晚接入使用已经审阅的
集成实现。Host 注册路由不会让未保留的信号编号变为 VM 独占。

使用独立启动/生命周期线程，其上下文零服务 Guest 监督线程的控制通道。应用 native 调用在其他已附着 VM 线程上执行，按后端获得同线程收养域或 worker 绑定，保留已有 owner/control 模型。

start 返回零或 errno。激活前，Host/服务协商私有传输版本、会话布局和必需能力，服务核对补丁 libc 的收养 ABI；普通或不匹配的 libc 会被拒绝。桥初始化开始后的失败使 runtime 隔离并保留资源；调用方必须丢弃进程，不能重试初始化。
校验失败后桥仍为 `GW_HOST_NEW` 时，可以修正配置后重试。runtime 先认领桥，再创建
分发线程或启用 Guest 故障路由，因此拒绝配置不会遗留这两类副作用。认领之后的终止性
失败不回滚辅助线程。

启动 GNU 代码前，接口创建真实 Host 分发线程，遵守
[信号转交契约](../../../core/docs/zh/HOST_SIGNAL_RELAY.md)。真正 GNU pthread 收到
路由的 Host 信号后，等待分发线程执行完处理器。两种后端均明确改变处理线程；
分发线程不会自动附着到 VM，也不能在信号处理器中调用 JNI。宿主已经启动分发线程时，
runtime 只借用它，关闭后保留其运行；否则 runtime 拥有该线程，在 Guest 控制停止后
join。两种情况都在请求未完成时拒绝关闭。外部所有者在 runtime 关闭之后、还原路由器
之前 join 借用的分发线程；外部发送者必须已静止。

shutdown 要求生命周期所有者、停止生产者、native 工作静止且模块已关闭。它不取消调用、不销毁 VM，也不卸载已保存栈/TLS/代码。空闲 VM 绑定和 GNU 监督线程保持固定。两个后端均遵守 [生命周期契约](LIFECYCLE.md)。

`build-guest.sh` 构建匹配服务；`scripts/package-runtime.sh` 将其与支持收养的 libc 及 domain-runtime.json 清单一同打包。`GW_DOMAIN_RUNTIME` 可显式选择匹配目录；打包器拒绝 ABI 或收养版本错误的清单。低层 full_host_start 仍供自备 worker 启动机制的宿主使用，不会静默创建 runtime。

`tests/runtime_sdk.py` 仅通过安装的 CMake 包和头构建消费者，检查默认/worker 选择、JNI 装载/回调/数组/TLS、生命周期与普通 libc 拒绝。依赖路径均由命令行显式提供；日志、清单与结构化结果在 android-jni/build/tests/runtime-sdk/。`tests/runtime_sdk_android.py` 使用显式审阅的 JRE 启动夹具/profile 在 ARM64 重复检查，并未把 VM 创建移入 android-jni；结果在 android-jni/build/tests/runtime-sdk-android/。

所有权分类已实现，但完整独立 Guest 信号语义仍待完成；见 [core 域边界](../../../core/docs/zh/DOMAINS.md)。默认同线程不取消这些限制。
