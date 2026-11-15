# Same-thread domain switching spike

这是隔离的纯 C 机制实验，不进入 core 安装目标或正常运行时包。它检验：在同一个内核线程中，能否用独立栈和独立 TLS 执行 Host → glibc Guest → Host 回调，并支持嵌套重入。

**当前结论：可以继续研究线程收养与信号路由；尚不能接入 JVM 或替代 worker 后端。** `result=PASS` 只代表下面的有限断言通过，`production_ready` 始终为 `false`。

## 实现边界

- Host 使用 core 已有的 bootstrap 映射匹配的 GNU loader、构造初始栈。Guest 的重定位、初始 TCB/TLS 和后续 `dlopen` 都由该 loader 完成，没有复制 TCB 或借用其他活跃 pthread 的 TLS。
- `switch.S` 保存调用约定要求的寄存器、栈、线程指针及浮点控制状态。x86_64 使用 `arch_prctl` 系统调用读写 FS；ARM64 使用 `TPIDR_EL0`。这里没有零系统调用或延迟收益承诺。
- Guest 从其 `main` 主动返回 Host 域。首次恢复后才 `dlopen(probe.so)`，验证后加载 DSO 的动态 TLS。
- 所有调用及回调固定在同一个 TID；回调也切换到 Host 栈与 TLS。嵌套调用复用两个真实 C 调用栈，不发送跨线程 RPC。
- **只有一个载体和一个 Guest 初始线程 TCB。尚未实现向已存在的 Guest runtime 收养多个 Host 线程。** 不能把此初始启动方式直接用于每个 JVM 线程。
- 整个程序由外部监督进程启动，完成后 `_exit` 丢弃全部资源。没有验证 Guest `pthread_exit`、取消、join、TLS 析构或 runtime 卸载。

## 断言与阻塞证据

每次运行执行 10,000 个最外层调用，每个调用递归到八层，共 80,000 次 Guest 函数调用、80,000 次 Host 回调和 70,000 次嵌套调用。断言包括：

1. Host/Guest 的实际 `gettid` 一致，线程指针与 TLS 地址不同。
2. Guest 栈地址处于独立保护页栈内，Host 回调栈处于其外。
3. 共享 POD 与指针可读写。Guest 分配内存，Host 回调读取，嵌套后由 Guest 释放；不跨 allocator 释放。
4. Host/Guest `errno`、可执行程序 `__thread`、后加载 DSO 的 TLS 初始化与计数保持正确。
5. Host 向下舍入、Guest 向上舍入，经过往返及重入后各自保持。

另外保留两个针对候选架构的反例检查，不将它们计为兼容通过：

**退出通知所有权。** `PR_GET_TID_ADDRESS` 读取 loader 启动前后的内核 clear-child-tid 地址。在本机 Linux x86_64 和 ARM64 QEMU 上观察到地址改变。匹配 glibc 2.44 源码的 `sysdeps/nptl/dl-tls_init_tp.c:__tls_init_tp` 调用 `set_tid_address(&pd->joinstate)`，因此正常 loader 启动会接管原载体的退出通知。只有在查询成功时，测试才恢复已知的原地址；这仍不构成完整生命周期协议。查询失败记录 errno，并将 `clear_tid_changed` 写为 `null`，不能解释成未改变。

**信号域错误。** Host 安装一个不读取 libc/TLS 的诊断处理器，分别在 Guest 和 Host 调用 `raise(SIGUSR1)`。它直接记录线程指针，确认 Host 安装的处理器在 Guest 执行时实际携带 Guest TLS。诊断处理器能安全记录不意味着普通 Host/VM 处理器可以直接运行。这不是信号虚拟化，也未验证异步切换窗口、同步 fault、`ucontext` 修改或嵌套信号。

## 复现

先按 [core 构建说明](../../README.md) 准备匹配的 glibc runtime。脚本会检查实际文件与编译器，不下载或改写正常 runtime。以下从仓库根目录运行：

```sh
# 本机 Linux x86_64，Host/Guest 为两套 glibc，不是 bionic 验证。
python3 core/experiments/same-thread/run.py

# ARM64 Linux 用户态 QEMU；显式填写本机实际 sysroot。
python3 core/experiments/same-thread/run.py \
  --abi arm64-v8a --qemu --sysroot /usr/aarch64-linux-gnu

# Android ARM64；填写实际 NDK 与在线设备序列号。
python3 core/experiments/same-thread/run.py \
  --abi arm64-v8a --ndk "$ANDROID_NDK" --serial "$ANDROID_SERIAL"
```

`--runtime` 可指定其他匹配 runtime；`--output` 可保留不同构建结果。只传 `--ndk` 会构建 Android Host，不宣称已运行。

Android Host 以 API 29 构建并禁用 emulated TLS，直接测试 ELF TLS，要求设备为 Android 10 或更新。runner 在 `/data/local/tmp/gw-same-thread-<manifest-hash>` 部署独立文件，通过 `/system/bin/linker64` 启动 bionic Host，不安装 APK。**这是 ADB shell 身份的真实 Android 双 libc 测试，不代表应用 UID/SELinux/seccomp 或 HotSpot 条件已通过。** 不要将它装载进正在运行的应用。

输出位于 `core/build/experiments/same-thread/<ABI>-<linux|android>/`：

- `manifest.json`：编译命令、源码/runtime/制品 SHA256、设备身份与部署目录。
- `stdout.log`、`stderr.log`：原始运行输出。
- `summary.json`：成功执行后的结构化断言与明确的未验证项；失败前清除旧 summary。

首轮检查结果：Linux x86_64、ARM64 QEMU、V2239A 真机的 bionic/glibc shell 进程均通过上述 80,000 次调用断言。真机的 clear-child-tid 查询不可用；所有平台都复现信号处理器继承 Guest TLS。QEMU 的 robust-list 查询不可用，亦不能作为兼容证据。每次重新运行应以自己的 summary 与对应 manifest 为准。

## 后续准入条件

在接入 HotSpot 前，必须完成：

1. 对现有 runtime 的受控线程收养入口。借用载体的 TID，明确 TCB/DTV、线程列表、clear-child-tid、rseq、robust-list、取消及析构所有权，不能靠复制 `struct pthread`。
2. 可恢复的信号域路由，覆盖切换中间态、同步 fault、VM 信号链及返回原域；不屏蔽硬件 fault 后再重放。
3. Host 线程退出、Guest 后台线程和嵌套回调的生命周期测试。不可在执行途中因超时迁移至其他线程。
4. 应用进程与真实 HotSpot 的 JNI 回归，然后才对比完整桥开销。

当前不支持跨域 unwind/longjmp、活跃 x86 CET shadow stack、任意多线程收养或 VM 私有线程状态。测试中八层深度是覆盖参数，不是新后端的容量承诺。
