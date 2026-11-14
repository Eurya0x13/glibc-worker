# glibc-worker core

不依赖 JNI、Java 或 libffi 的运行时基础组件。源码包含固定 GNU loader 的 PT_LOAD 映射与启动栈、raw clone 架构入口、futex 事件和 RPC 诊断原语。GNU DSO 重定位和 TLS 初始化继续由现有 GNU loader 完成。

`src/bootstrap/` 保存架构启动源码；`include/` 保存协议与运行时原语。

## 独立构建

```sh
./build.sh -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
ctest --test-dir build --output-on-failure
```

默认输出只在本项目 `build/`。Android 构建通过 CMake 的 NDK toolchain、`ANDROID_ABI` 与 `ANDROID_PLATFORM` 选择目标；不需要 JDK、JNI 头或其他项目源码。安装包为 `GlibcWorkerCore`，目标为 `glibc_worker::core`。

匹配的 GNU loader/libc 由 `scripts/build-glibc.sh` 构建；Host/Guest libffi 由 `scripts/build-ffi.sh /path/to/ndk-clang` 构建。两项都是可选依赖工具，不进入基础 core 库的链接依赖。配置和 Termux 补丁都归本项目。下载在 `vendor/`、源码缓存在 `work/`、runtime 在 `build/glibc-runtime/<ABI>/`，libffi 在 `build/ffi-{android,linux}/<ABI>/`。runtime 发布匹配的 loader、libc、libpthread、libm、librt 和 libdl；调用者不读取 glibc 的内部构建树。

## 测试与边界

`tests/` 自己维护真实 futex、通知竞态、配对交接、嵌套统计和受控时钟故障测试；需要 Clang ASan/UBSan 与 Python，输出在本项目 `build/test-work/`。CMake 配置 `-DBUILD_TESTING=OFF` 可只构建组件。

core 提供低层 bootstrap，不提供任意 runtime 的一键启动/销毁 API。宿主仍负责部署、栈与共享区生命周期、信号策略、启动参数和线程退出确认；GNU 映射当前保留到进程退出。core 不管理 JavaVM，也不混用两套 libc 的 allocator、FILE 或 pthread 对象。

等待与诊断设计见 [WAITING](docs/WAITING.md)，实验性通知过滤见 [WAKE_FILTER](docs/WAKE_FILTER.md)。JNI 上下文配对与对象协议属于上层 android-jni。

GNU runtime 构建脚本会核对源码补丁；忽略目录或父级 Git checkout 不能使补丁被静默跳过。构建与缓存验证契约见 [RUNTIME_PATCHES](docs/RUNTIME_PATCHES.md)。
