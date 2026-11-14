# 工作区组合构建

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

`GW_CONTEXTS` 默认 16（2..256）、`GW_RPC_PROFILE=0`、`GW_RPC_SPIN_NS=20000`、`GW_FUTEX_WAKE_FILTER=0`、`GW_RPC_READY_POLL=0`。每个项目保存自己的 abi-flags，Host/Guest 必须使用同一份布局和配置。

`GW_JRE_ADAPTER_HEADER` 仅用于 jvm-jni，必须显式选择匹配运行中 libjava 的 profile。未配置不能请求私有钩子。`GW_RUNTIME_OUT` 仅重定向上层兼容制品，不改变各项目自己的编译/安装目录。

旧单 DSO 集成使用 `jvm-jni/build/<ABI>/compat/` 的 `libglibc_bridge.so`、GNU service、helpers 与 bootstrap；模块化集成使用已安装的四个包。不要在同一进程同时加载两种组合，避免重复的单例状态。

真实 VM 冒烟/覆盖入口归 [jvm-jni](../jvm-jni/README.md)。Pojav 集成不属于任何核心项目。
