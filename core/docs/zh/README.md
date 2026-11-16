# glibc-worker core

[English](../en/README.md) | [简体中文](../zh/README.md)

不依赖 JNI、Java 或 libffi 的运行时基础组件。源码包含固定 GNU loader 的 PT_LOAD 映射与启动栈、raw clone 架构入口、futex 事件和 RPC 诊断原语。GNU DSO 重定位和 TLS 初始化继续由现有 GNU loader 完成。

`src/bootstrap/` 保存 loader 启动源码；`src/domain/` 保存同线程域切换和信号路由；`src/runtime/` 保存 GNU 收养补丁；`include/` 保存协议与运行时原语。

## 独立构建

```sh
./build.sh -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
ctest --test-dir build --output-on-failure
```

默认输出只在本项目 `build/`。Android 构建通过 CMake 的 NDK toolchain、`ANDROID_ABI` 与 `ANDROID_PLATFORM` 选择目标；不需要 JDK、JNI 头或其他项目源码。安装包为 `GlibcWorkerCore`，目标包括 `glibc_worker::core`、`glibc_worker::domains` 和显式选择的 `glibc_worker::signal_interpose`。详见 [同线程域与信号边界](DOMAINS.md)。

匹配的 GNU loader/libc 由 `scripts/build-glibc.sh` 构建；Host/Guest libffi 由 `scripts/build-ffi.sh /path/to/ndk-clang` 构建。两项都是可选依赖工具，不进入基础 core 库的链接依赖。配置和 Termux 补丁都归本项目。下载在 `vendor/`、源码缓存在 `work/`、runtime 在 `build/glibc-runtime/<ABI>/`，libffi 在 `build/ffi-{android,linux}/<ABI>/`。runtime 发布匹配的 loader、libc、libpthread、libm、librt 和 libdl；调用者不读取 glibc 的内部构建树。

显式 Host 信道通过 `signal_channel.h` 在 VM 启动前配置，保留调用者指定的 RT 编号，
适配 Host 待决查询、消费等待及 signalfd 后端，详见 [信号等待](SIGNAL_WAITING.md)。
它不会自动启用 Guest 独立异步投递。

## 测试与边界

`tests/` 自己维护真实 futex、通知竞态、配对交接、嵌套统计和受控时钟故障测试；需要 Clang ASan/UBSan 与 Python，输出在本项目 `build/test-work/`。CMake 配置 `-DBUILD_TESTING=OFF` 可只构建组件。

`gw_bootstrap_stack` 只接受 1..16 个非空参数指针；在任何栈写入前检查地址溢出、参数/固定环境/auxv/随机字节与对齐预算，并保留至少 4 KiB 余量。原有低层包装仍按契约终止进程；`gw_bootstrap_map_checked` 和 `gw_bootstrap_stack_checked` 返回 errno 值，失败时不改变输出，映射失败会清理临时资源。`tests/audit_boundaries.py` 通过保护页、共享哨兵和子进程验证拒绝前未写栈，并验证有效 argv/env/auxv。

`rpc_sequence.h` 将有符号原子序号的下一值明确为 `INT_MAX → INT_MIN`，避免 C 有符号溢出；零同样是有效序号。它是每槽最多一个在途请求的等值令牌，不可用于大小排序或并发生产者分配。边界测试在四种通知/轮询配置下执行真实 pthread/futex 请求与响应，覆盖两处回绕。

core 提供低层 bootstrap，不提供任意 runtime 的一键启动/销毁 API。宿主仍负责部署、栈与共享区生命周期、信号策略、启动参数和线程退出确认；GNU 映射当前保留到进程退出。超时或通知唤醒不等于对端已经停止访问共享区/借用地址；未确认线程退出前不得释放其栈和映像。若宿主选择丢弃执行进程，应由其监督路径处理，core 不取消任意函数或自动结束宿主。core 不管理 JavaVM，也不混用两套 libc 的 allocator、FILE 或 pthread 对象。

等待与诊断设计见 [WAITING](WAITING.md)，实验性通知过滤见 [WAKE_FILTER](WAKE_FILTER.md)。JNI 上下文配对与对象协议属于上层 android-jni。

GNU runtime 构建脚本会核对源码补丁；忽略目录或父级 Git checkout 不能使补丁被静默跳过。构建与缓存验证契约见 [RUNTIME_PATCHES](RUNTIME_PATCHES.md)。

GNU robust mutex 初始化明确返回 ENOTSUP，避免关闭内核 robust-list 登记后仍报告创建成功。普通锁与条件变量保持原实现；边界、原因及 `tests/mutex_contract.py` 的主机正对照/指定 runtime 验证见 [mutex 契约](MUTEXES.md)。
