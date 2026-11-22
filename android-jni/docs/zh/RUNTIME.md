# Runtime 启动接口

[English](../en/RUNTIME.md) | [简体中文](../zh/RUNTIME.md)

安装的 `runtime_bridge.h` 提供 `gw_host_runtime_start` 与 `gw_host_runtime_shutdown`，使用已安装的 core 域目标和正常 JNI 桥，不依赖实验头文件或测试可执行程序。

启动配置带版本并要求显式输入：

- backend：零选择同线程，`GW_EXECUTION_WORKER` 选择 worker。
- loader_path、library_path、service_path：由宿主部署的匹配 GNU runtime 和服务。
- bridge：已有映像策略、JNI flags 和可选已注册 VM adapter。
- signals：与已准备 Host 信号路由器匹配的不可变所有权契约。

worker 后端进入兼容性维护模式。接入方可显式选择 `GW_EXECUTION_WORKER`，
用于兼容既有环境或诊断对照。worker 继续接受正确性和兼容性修复，并保留 SDK
回归覆盖。新增执行能力和性能优化面向默认的同线程后端。

启动器提供实际 JVM 线程上的 `JNIEnv`。加载 VM 前在原生主程序中完成
[Host 启动期信号路由](../../../core/docs/zh/SIGNAL_BOOTSTRAP.md)。可选通道配好后，
用 `gw_host_signal_contract_init()` 填充 `signals`。启动接口在准入 Guest 代码前
校验 Host 注册和所有权契约。

使用独立启动/生命周期线程，其上下文零服务 Guest 监督线程的控制通道。应用 native 调用在其他已附着 VM 线程上执行，按后端获得同线程收养域或 worker 绑定，保留已有 owner/control 模型。

start 返回零或 errno。启动 GNU 代码前，Host 打开服务文件并核对 ELF note 中的
私有传输版本，再通过 `/proc/self/fd` 将同一个已打开文件交给动态链接器。
监督线程初始屏蔽可捕获信号，校验共享布局、必需能力和补丁 libc ABI 后，
才注册面向 Host 的信号入口并应用继承的掩码。桥初始化开始后的失败使 runtime
隔离并保留资源，调用方必须丢弃进程。
校验失败后桥仍为 `GW_HOST_NEW` 时，可以修正配置后重试。runtime 先认领桥，再创建
分发线程或启用 Guest 故障路由，因此拒绝配置不会遗留这两类副作用。启动与关闭
调用重叠时返回 `EBUSY`，此时不会访问传输状态。桥认领后的终止性失败保留辅助线程。

启动 GNU 代码前，接口创建真实 Host 分发线程，遵守
[信号转交契约](../../../core/docs/zh/HOST_SIGNAL_RELAY.md)。真正 GNU pthread 收到
路由的 Host 信号后，等待分发线程执行完处理器。两种后端均明确改变处理线程；
分发线程不会自动附着到 VM，也不能在信号处理器中调用 JNI。宿主已经启动分发线程时，
runtime 只借用它，关闭后保留其运行；否则 runtime 拥有该线程，在 Guest 控制停止后
join。两种情况都在请求未完成时拒绝关闭。外部所有者在 runtime 关闭之后、还原路由器
之前 join 借用的分发线程；外部发送者必须已静止。

shutdown 要求生命周期所有者、停止生产者、native 工作静止且模块已关闭。它不取消
调用、不销毁 VM，也不卸载已保存栈/TLS/代码。空闲 VM 绑定和 GNU 监督线程保持固定。
Guest 信号描述符仍打开或 Host 描述符等待仍在进行时，在关闭桥之前返回 `EBUSY`。
关闭最后一个描述符引用后可重试。成功关闭后的重复调用返回 `EALREADY`。两个后端
均遵守 [生命周期契约](LIFECYCLE.md)。

确认 STOP 前，监督线程 join 自己的 worker，通过原生内核掩码屏蔽所有可捕获
信号，并撤销自己的 Host 信号注册表入口。随后永久停在原生 futex 中，不再轮询
应用命令。Host 在停止自己的分派线程前核对已确认的掩码，避免恢复路由器后，
空闲 GNU 监督线程进入原生 Host 处理器。`SIGKILL`/`SIGSTOP` 保留原生内核语义；
这不代表完整的 VM/ART 接收者静默，也不允许解除 GNU runtime 映射。诊断线程名
为 `gw-guest-owner`。就绪、描述符预检和 STOP 确认等待各使用 15 秒单调时钟截止时间。
桥提交关闭后，如果 STOP 或分发线程 join 失败，返回终止性 `EIO` 并保留剩余资源。
私有传输支持描述符预检和静默 STOP 确认，旧服务在激活前被拒绝。

`build-guest.sh` 构建匹配服务；`scripts/package-runtime.sh` 将其与支持收养的 libc 及
`domain-runtime.json` 清单一同打包。`GW_DOMAIN_RUNTIME` 可显式选择匹配目录；打包器拒绝 ABI
或收养版本错误的清单。

### 打包后的 glibc 库名

打包时校验每个内置 glibc 库的架构与完整 `DT_SONAME`。`domain-runtime.json` 中的
`glibc_aliases` 记录 ABI 库名及其打包文件。两个 ABI 的 `libdl.so.2` 均对应
`libguest_dl.so`。解包使用时，将 `compat/glibc-libs` 加入 `library_path`。移动制品时保留
目录中的相对符号链接。

打 APK 时，将匹配的 `libguest_*.so` 部署为安装时解包的原生库，并保持文件内容不变。
将对应 ABI 的 `glibc-libs.tsv` 加入 assets。宿主启动运行时前，调用
`GuestLibraries.prepare(privateStorage, nativeLibraryDir, catalogStream)`。辅助类按清单校验已
安装文件，在应用私有目录中创建可复用的链接。把返回目录加入 `library_path`，
`loader_path` 和 `service_path` 继续使用已安装的 loader 和服务路径。

GNU loader 通过搜索路径处理裸库名和 `DT_NEEDED`。按路径装载时，使用实际存在的别名路径，
例如 `<别名目录>/libdl.so.2`。内置别名采用完整 ABI 名称。应用按需提供其他依赖，GNU loader
按宿主配置的目录及依赖的 RUNPATH 查找。

随包发布的 GNU loader 为带有 RUNPATH 或适用 RPATH 的 Guest DSO 解析 `DT_NEEDED` 时，
会在按库名复用已装载的依赖前，按原有优先级检查配置的搜索路径。如果搜索结果是另一份文件，
本次装载会在新映像的构造函数执行前被拒绝。诊断列出已装载文件、候选文件以及已知的请求者。
诊断超出桥接缓冲区时，完整文本会写入 Guest stderr。解析到相同设备号和 inode，或配置路径
中找不到候选时，可以复用已装载的依赖。

下面的回归使用打包的 GNU loader 和辅助 JAR。在 x86_64 上测试 arm64-v8a 制品时，使用
GNU 交叉编译器，并附加 `--runner qemu-aarch64`。

```sh
python3 android-jni/tests/runtime-aliases.py --package "$GW_PACKAGE" \
  --helpers "$GW_PACKAGE/glibc-bridge-helpers.jar"
```

低层 `full_host_start` API 仍供自备 worker 启动机制的宿主使用。

`tests/runtime_sdk.py` 仅通过安装的 CMake 包和头构建消费者，检查默认/worker 选择、JNI 装载/回调/数组/TLS、生命周期与普通 libc 拒绝。依赖路径均由命令行显式提供；日志、清单与结构化结果在 android-jni/build/tests/runtime-sdk/。`tests/runtime_sdk_android.py` 使用显式审阅的 JRE 启动夹具/profile 在 ARM64 重复检查，并未把 VM 创建移入 android-jni；结果在 android-jni/build/tests/runtime-sdk-android/。

关闭测试除 API 返回值外，还通过 `/proc/self/task` 检查监督线程实际的内核
信号掩码与 futex 等待。生产运行时不依赖这些 `/proc` 观测。

普通服务执行 Host 信号保留策略。跨 libc 域的独立 Guest 投递需要下述显式配置。
参见 [core 信号所有权](../../../core/docs/zh/DOMAINS.md)。

可选 unwind 服务为同步故障使用所属 Guest 的
[处置快照](../../../core/docs/zh/SIGNAL_ACTIONS.md)。重新注册不再分配固定的记录，
正在执行的投递保留自己的完整处置副本。

启用可选 native unwind 构建后，V2 配置可在 `guest_notification`
显式指定一个未占用的公开 RT 编号，已占用的编号会使准入失败。匹配的 Guest
服务在 clone 前预留 pthread 身份，在创建返回前发布 TID，并把带标签的消息
路由到所属 GNU 信号入口。普通 Host 消息仍走 Host 分派线程。GNU 63/64 的取消
和凭据处理器保留原生注册。STOP 确认所有应用接收者退休后才释放 Guest 注册表。

同线程后端还要求在启动时准备好 Host 延迟信号通道及原生展开拦截。载体运行
Java、在 Host libc 内等待或执行转发的 Host 处理器时，Guest 接收者使用真实
收养的 GNU TCB。嵌套入口使用最近的 Guest 帧，并在切换前记录活动栈边界。
见 [Guest 异步入口](../../../core/docs/zh/GUEST_ASYNC_ENTRY.md)。

带标签发送、处置和屏蔽集合消费使用匹配的 Guest libc ABI。Guest signalfd 支持
就绪通知、描述符别名和并发关闭。作业控制信号按 Linux 原生语义发送，使用由 Host
拥有的共享处置。Guest 尝试改写作业控制处置会收到 `EBUSY`。信号等待契约覆盖
Host 超时与重启行为，以及私有入口、直接系统调用的原生限制。参见
[信号等待](../../../core/docs/zh/SIGNAL_WAITING.md)。

受支持的原生转移可以恢复保存的上下文，并清理被放弃路径中由桥持有的 JNI 资源。
契约要求原生目标有效，并保留仍存活的 Java 帧。参见
[非局部恢复](../../../core/docs/zh/NONLOCAL_RECOVERY.md) 与
[已安装 SDK 测试](SDK_TESTS.md)。
