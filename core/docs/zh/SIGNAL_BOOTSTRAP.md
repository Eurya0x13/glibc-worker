# Host 启动期信号路由

[English](../en/SIGNAL_BOOTSTRAP.md) | [简体中文](../zh/SIGNAL_BOOTSTRAP.md)

将 `glibc_worker::signal_bootstrap` 链接进原生 Host 可执行程序。它从主程序导出
信号注册入口，在 Host 启动 VM 或执行 Guest 代码前接管继承的信号动作。同一套接口
支持原生 GNU/Linux 和 Android Host，无需提供 ROM 制品散列或导入槽偏移。

在 Android 上，启动器可以通过 Activity 管理原生子进程，以应用 UID 运行该程序。
将可执行程序随 APK 原生库打包。已验证的 ARM64 环境通过
`/system/bin/linker64` 启动安装后的程序。HotSpot 和 glibc Guest 共享子进程的
地址空间，Android 界面在主管进程的 ART 中运行。

## 链接与启动

使用 HotSpot `System.load` 适配器的启动器，从安装后的 CMake 包将 JNI 层静态链接
进同一个可执行程序。

```cmake
find_package(GlibcWorkerJvmJni CONFIG REQUIRED)
target_link_libraries(native_host PRIVATE
  glibc_worker::signal_bootstrap glibc_worker::jvm_jni_static)
```

静态 JVM target 会引入静态的 android-jni 和 bionic-dlopen。只接入公开 JNI 桥时，
使用 `glibc_worker::android_jni_static`。一个进程使用一套 SDK 组合，让 Guest
准入、信号注册和关闭共用同一个路由器。

1. 在加载 JVM 或创建其他应用线程前，调用 `signal_bootstrap.h` 中的
   `gw_host_signal_bootstrap()`。该入口检查 Host 导出的注册符号是否生效。
   若已按标准库名加载 ART、libsigchain 或 HotSpot，或注册符号无法拦截，返回
   `ENOTSUP`。
2. 通过 [`signal_channel.h`](SIGNAL_WAITING.md) 配置所需的可选 Host 通知通道，
   然后启动启动器选定的 JVM。
3. 配好通道后调用 `gw_host_signal_contract_init(&signals)`。将得到的契约填入
   `gw_host_runtime_config.signals`，用该 JVM 的真实 `JNIEnv` 启动
   [JNI runtime](../../../android-jni/docs/zh/RUNTIME.md)。
4. 等待调用结束，关闭模块并停止 JNI runtime。销毁 JVM，确保信号接收线程静止后，
   在启动线程调用 `gw_host_signal_restore()`。运行时映射保留到进程退出。

启动和关闭接口返回零或 errno 值。`EALREADY` 表示路由器已启动。`EIO` 需要由
主管进程丢弃子进程。关闭时遇到 `EBUSY`，可待尚在使用的资源静止后重试。Host
须通过主程序导出的入口注册受管信号。裸系统调用和从 libc 句柄直接取得的函数
指针会绕过该路由。

## JVM 接入由启动器维护

**启动器自行维护 JVM 侧接入。** 启动器负责选定 Android JRE 制品、按制品要求
布置目录、传入正确参数创建 JVM，并维护对应制品的原生库装载路径。使用 jvm-jni
代理 `System.load` 时，启动器提供已审阅的 load/find/unload 接口族和确切的
libjava/libjvm 制品身份。这些信息对应所选 JRE 的私有装载接口。
参见 [JVM 适配契约](../../../jvm-jni/docs/zh/ADAPTER.md)。

[`System.load` 夹具](../../../jvm-jni/docs/zh/SYSTEM_LOAD.md)覆盖 Android 应用
UID 下的启动、bionic 私有信号注册和同线程 JNI。core 的
`signal_bootstrap` CTest 覆盖继承动作、已加载的外部信号链及动作恢复。
