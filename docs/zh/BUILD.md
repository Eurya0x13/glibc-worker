# 工作区组合构建

[English](../en/BUILD.md) | [简体中文](../zh/BUILD.md)

四个项目各自维护 CMake 构建、安装包、测试和 build 目录。各项目 README 提供独立构建方法。工作区脚本只是依赖顺序与现有宿主兼容制品的组合器。

```sh
GW_ABI=arm64-v8a scripts/build-runtime.sh --module core
GW_ABI=arm64-v8a scripts/build-runtime.sh --module bionic-dlopen
GW_ABI=arm64-v8a scripts/build-runtime.sh --module android-jni
GW_ABI=arm64-v8a scripts/build-runtime.sh --module jvm-jni
```

默认 `all` 与 `jvm-jni` 相同。每项的对象、库、生成头、辅助 JAR、CMake 安装包在 `<project>/build/<ABI>/`。依赖通过各自 `install/` 的 CMake 包读取；没有公共安装前缀，也不在下层拼入上层源码。

core 的 GNU 运行时在 `core/build/glibc-runtime/<ABI>/`，两侧 libffi 在 `core/build/ffi-{android,linux}/<ABI>/`；android-jni 的 JNI 生成物在 `android-jni/build/`；Android 测试应用、测试 JRE 与集成结果在 `jvm-jni/build/`。根目录不保留 build/work/vendor。格式工具的缓存单独在 `scripts/build/tools/`。

`core`、`bionic-dlopen` 的组合构建只需要 Python、CMake、NDK；不调用 javac，不读取 JNI 头，不构建 libffi/JRE。JNI 以上需要 JDK、匹配的 GNU 工具链、glibc/libffi。NDK 仍由环境变量或工作区 local.properties 中的 sdk.dir 发现，不猜安装路径。

`GW_CONTEXTS` 是初始上下文容量，默认 16（构建范围 2..256，运行时按 16 项动态增长）、`GW_RPC_PROFILE=0`、`GW_RPC_SPIN_NS=20000`、`GW_FUTEX_WAKE_FILTER=0`、`GW_RPC_READY_POLL=0`。每个项目保存自己的 abi-flags，Host/Guest 必须使用同一份布局和配置。

android-jni 的 Host 资源存储固定使用 Zig 0.16.0，构建机器必须提供该版本；安装后的 C/C++ SDK 使用者无需 Zig。`GW_BINDING_BACKEND` 可以省略，显式 `zig` 仍可用，`c` 被明确拒绝；旧 CMake 缓存若保存了 `c`，需要清除缓存或改为 `zig`。初始存储容量为 512 个 binding、16,384 个引用/ID 槽、256 个注册类记录，耗尽后动态增长；模块上限固定 64。配置与释放约定见 [Host 资源管理](../../android-jni/docs/zh/ZIG_BINDINGS.md)。

`GW_JRE_ADAPTER_HEADER` 仅用于 jvm-jni，必须显式选择匹配运行中 libjava 的 profile。未配置不能请求私有钩子。`GW_RUNTIME_OUT` 仅重定向上层兼容制品，不改变各项目自己的编译/安装目录。

旧单 DSO 集成使用 `jvm-jni/build/<ABI>/compat/` 的 `libglibc_bridge.so`、GNU service、helpers 与 bootstrap；模块化集成使用已安装的四个包。不要在同一进程同时加载两种组合，避免重复的单例状态。

真实 VM 冒烟/覆盖入口归 [jvm-jni](../../jvm-jni/docs/zh/README.md)。Pojav 集成不属于任何核心项目。

## 清理生成缓存

先预览过期的 glibc 源码和对象缓存，再按相同策略执行清理：

```sh
python3 scripts/clean-cache.py
python3 scripts/clean-cache.py --apply --json core/build/cleanup-result.json
```

脚本保留各 ABI 当前普通运行时的源码树，以及最近发布的一代同线程运行时源码树。
`--keep-domain-builds 2` 可让每种 ABI 保留两代；
`--keep core/work/<build-directory>` 可指定额外保留的源码树。
只有 `core/work/` 下格式匹配、受 Git 忽略的构建缓存才可删除；包含受版本控制文件、
正在使用的构建树和根目录符号链接都会拒绝。请在没有构建任务时运行；进程检查无法
阻止其他构建在检查之后启动。

已发布的运行时二进制、libffi、JRE、下载依赖和测试结果均保留。旧运行时再次构建时
可能需要重新生成被删除的源码和对象。JSON 报告逐项记录删除结果，中途失败也保留
已完成的进度。不加 `--apply` 时不会删除文件。

## 源码布局与构建迁移

实现目录为 `core/src/bootstrap/`、`bionic-dlopen/src/`、`android-jni/src/{host,guest,common,java}/`、`jvm-jni/src/{native,java}/`。JVM 公开适配头文件在 `jvm-jni/include/`；安装后的包含路径不因内部搬迁而变化。生成源码写入各项目 build，不回写 src。

Zig 编译规则由 `android-jni/build.zig` 维护；CMake 直接调用 `zig build host-store`，测试脚本也调用同一构建图。Python 不再维护另一份 Zig 编译参数。后续迁移可按各项目的编译、链接、安装边界推进；依赖获取、JNI 表生成、设备测试和报告处理不因迁移构建图而必须改写为 Zig。
