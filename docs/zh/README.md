# glibc-worker workspace

[English](../en/README.md) | [简体中文](../zh/README.md)

在 Android bionic 进程中运行独立的 GNU libc 线程域。这个工作区包含四个独立子项目；Java/HotSpot 是可选接入层。

| 项目 | 职责 | 依赖 |
| --- | --- | --- |
| [core](../../core/docs/zh/README.md) | 固定 GNU loader 的 bootstrap、架构入口、事件通知与 RPC 诊断原语 | Linux/Android 系统接口，无 JNI/Java |
| [bionic-dlopen](../../bionic-dlopen/docs/zh/README.md) | C 装载契约、ELF 路由、句柄所有权、装卸载同步 | core，无 JNI/Java |
| [android-jni](../../android-jni/docs/zh/README.md) | 公开 JNI 的 Host/Guest 桥、引用/异常/回调、GNU service | core、bionic-dlopen、两侧 libffi |
| [jvm-jni](../../jvm-jni/docs/zh/README.md) | 可选的 HotSpot/JRE 私有装载适配与真实 JRE 集成测试 | android-jni |

每个项目的实现源码统一放在 `src/`；公开接口放在 `include/`，测试留在 `tests/`。每个项目都有自己的 `CMakeLists.txt`、`build.sh`、`docs/` 和 `build/`。它们可以分别构建和安装；通过安装的 CMake 包和头文件声明依赖。没有公共 `build/` 或公共测试目录。

`android-jni/build.zig` 负责必需的 Zig Host 存储对象的编译；CMake 继续负责跨项目依赖、C/JNI 和 SDK 安装。尚未将四个项目的构建全部迁移到 Zig。

根目录 `scripts/` 只提供工作区组合构建、工具链发现、格式和导出。组合入口调用各项目的构建入口；制品仍归各自项目：

```sh
GW_ABI=arm64-v8a scripts/build-runtime.sh --module core
GW_ABI=arm64-v8a scripts/build-runtime.sh --module bionic-dlopen
GW_ABI=arm64-v8a scripts/build-runtime.sh --module android-jni
GW_ABI=arm64-v8a scripts/build-runtime.sh --module jvm-jni
```

上层选择会按顺序构建所需依赖。也可直接使用各项目的 `build.sh`，参见各自 README 与 [组合构建](BUILD.md)。

`jvm-jni` 的组合构建额外提供旧宿主使用的单文件 `libglibc_bridge.so` 和辅助 JAR。这是上层兼容制品，不属于 core，也不能与模块化 DSOs 在同一个进程重复加载。

当前真实验证仍主要覆盖 Android HotSpot。公开 JNI 层已与 JRE 私有接口分离，这不等于 ART 的完整 JNI 桥已验证。Pojav 与真实模组接入在外部验证工程，本工作区不包含启动器源码。

依赖下载和源码缓存分别归所属项目的 `vendor/`、`work/`，构建输出归各自 `build/`；根目录不保存这些缓存。代码格式见 [STYLE](STYLE.md)，项目边界、契约文档与 review 顺序见 [REVIEW_GUIDE](REVIEW_GUIDE.md)。

计划中的工作见 [项目待办](TODO.md)。

## 同线程域

加载 JVM 前在原生主程序中完成[启动期信号路由](../../core/docs/zh/SIGNAL_BOOTSTRAP.md)。JVM 侧接入由启动器维护。

core 维护同线程域切换、收养补丁和信号路由。android-jni 的 `runtime_bridge.h` 启动接口默认
同线程。worker 进入兼容性维护模式，接入方仍可显式选择该后端。各项目的同线程夹具、
生成结果和已复核历史记录分别位于 `tests/same-thread/`、`build/tests/` 和
`tests/results/`。参见
[core 域机制与信号边界](../../core/docs/zh/DOMAINS.md) 和
[JNI runtime 接入](../../android-jni/docs/zh/RUNTIME.md)。

[测量指南](../../jvm-jni/docs/zh/PERFORMANCE.md) 说明固定工作负载和计算方法。
[1–2 MiB 缓存验证](../../android-jni/tests/results/same-thread/array-cache/README.md)
覆盖数组复制与分配器退页。[数组副本契约](../../android-jni/docs/zh/ARRAY_COPY_CACHE.md)
说明调用者义务。结果适用于已验证的固定 ART、JRE 和模组制品。
