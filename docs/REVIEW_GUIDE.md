# 项目边界与 review 顺序

实现边界按所属项目维护：

- core：[运行时补丁](../core/docs/RUNTIME_PATCHES.md)、[mutex 契约](../core/docs/MUTEXES.md)。
- bionic-dlopen：[执行借用与关闭规则](../bionic-dlopen/docs/EXECUTION_SCOPES.md)。
- android-jni：[资源容量与增长](../android-jni/docs/ZIG_BINDINGS.md)、[上下文与所有权](../android-jni/docs/CONTEXTS_AND_OWNERSHIP.md)、[生命周期](../android-jni/docs/LIFECYCLE.md)、[失败语义](../android-jni/docs/FAILURES.md)。
- jvm-jni：[适配器契约](../jvm-jni/docs/ADAPTER.md)、[JRE 兼容性与已知限制](../jvm-jni/docs/JRE_COMPATIBILITY.md)。

1. `core/`：bootstrap、raw clone、futex 事件和诊断。检查这里没有 JNI、Java 或 libffi 依赖。
2. `bionic-dlopen/`：C transport、ELF 分类、逻辑句柄及装卸载同步。JNI 版本与方法描述符不在这里解释。
3. `android-jni/`：两侧 JNI 转发、引用/异常/线程/回调与 JNI loader 适配；通过可选钩子表使用 VM 扩展。
4. `jvm-jni/`：唯一知道 NativeLibraries 和固定 libjava profile 的项目，以及真实 Android JRE 的集成宿主。

每个项目都有独立 CMake 包、构建入口、测试与文档。依赖从安装产物传递；下层不 include 上层的私有头。根目录只负责导航、组合、格式与打包。

保留的 JNI v1/v2 接口与共享协议维持兼容。单文件 libglibc_bridge.so 只是 jvm-jni 的兼容组合，不能反过来作为 core 的定义。

core 与 bionic-dlopen 的本地测试分别通过各自 CTest 执行；真实 Android/JRE 的复现方法与通过标准见 [测试手册](../jvm-jni/tests/README.md)。运行结果写入所属项目 build，不放进开发者接口文档。
