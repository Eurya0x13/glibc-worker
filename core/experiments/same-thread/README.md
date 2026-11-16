# Same-thread domain switching spike

这是隔离的纯 C 机制实验，不进入 core 安装目标或正常运行时包。它检验：在同一个内核线程中，能否用独立栈和独立 TLS 执行 Host → glibc Guest → Host 回调，并支持嵌套重入。

**当前结论：初始域切换、多个 Host 线程收养和限定信号路由已有独立夹具；尚不能替代 worker 后端。** `result=PASS` 只代表对应夹具的有限断言通过，`production_ready` 始终为 `false`。真实 HotSpot 的有限集成测试位于 [jvm-jni 实验](../../../jvm-jni/experiments/same-thread/README.md)；Host 所有权信号链和生产 JNI 路径的动态上下文、回调及线程退出覆盖见 [android-jni 实验](../../../android-jni/experiments/same-thread/README.md)。双域独立信号语义仍未实现。

## 初始 loader 夹具的实现边界

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

先按 [core 构建说明](../../docs/zh/README.md) 准备匹配的 glibc runtime。脚本会检查实际文件与编译器，不下载或改写正常 runtime。以下从仓库根目录运行：

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

## 多载体收养夹具

`--adopt` 使用另一套 Host/Guest，区别于上述初始 loader 夹具。一个专用 Guest owner 线程正常启动 GNU loader；随后借助实验性 glibc 内部入口，为每个 Host pthread 分配自己的 TCB、DTV 和带保护页的栈。载体执行 Guest 时保留原来的内核 TID，不接管 Host 的 clear-child-tid、robust-list 或 rseq 注册。

`core/scripts/build-domain-runtime.py` 从 core 已准备的匹配 glibc 源码复制出独立构建树，将 `adopt-nptl.inc` 注入 NPTL。它不修改正常 runtime 或安装目标。内部入口分为分配、绑定载体、运行服务、执行线程析构、owner 回收五个阶段。未完成析构的 TCB 回收返回 `EBUSY`。这是依赖 glibc 内部结构的实验接口，不是稳定 API。

夹具使用 8 个并发载体、3 批线程生命周期，每个载体执行 100 次八层嵌套调用，总计 19,200 次 Guest 调用。覆盖独立 TLS、动态 DSO TLS、errno、浮点状态、指针传递、普通 Guest 后台 pthread 创建与 join、Host join，以及每载体各一次 pthread key 析构、TLS 析构和 cleanup handler。半数载体在最外层服务边界执行 Guest `pthread_exit`，只结束 Guest 服务，不退出 Host 内核线程。

收养载体上的取消明确返回 `ENOTSUP`。`--credential-guard` 还验证存在收养载体时 Guest `setuid(getuid())` 被 NPTL 防护拒绝并返回 `ENOTSUP`，不尝试改变进程凭据。这是显式不支持的语义，不应计为完整 pthread/POSIX 兼容。嵌套 Host 回调期间的 Guest `pthread_exit`、分配后尚未绑定的失败清理、完整 runtime 卸载仍未覆盖。

```sh
python3 core/scripts/build-domain-runtime.py --abi arm64-v8a
# 使用上一命令实际输出的目录，不猜测构建哈希。
python3 core/experiments/same-thread/run.py \
  --abi arm64-v8a --adopt --signals --credential-guard \
  --runtime "$DOMAIN_RUNTIME" --ndk "$ANDROID_NDK" \
  --serial "$ANDROID_SERIAL"
```

本机 x86_64 可同样构建 `--abi x86_64` 的实验 runtime，再省略 `--ndk`、`--serial` 运行。ARM64 Linux QEMU 使用 `--qemu --sysroot`，不能将其结果解释为 Android 应用兼容。

### 限定信号路由

`--signals` 在登记的载体线程上安装替代信号栈。汇编入口先以无 TLS/libc 依赖的方式查找载体，恢复 Host TP（ARM64 同时恢复 x18），再进入 C 处理器；退出后恢复被中断域。覆盖 Linux 实时信号 50、处理器内嵌套信号 51，以及 Host/Guest 各一次明确识别的 SIGSEGV 故障指令和 PC 恢复，共 48 次同步故障恢复。安装前检查两个测试通知信号未被占用，不覆盖 HotSpot 的 SIGUSR2。

**这不是通用信号链。** 夹具拥有这三个信号，固定登记 8 个载体；未识别故障或未登记线程进入该入口会终止测试进程。不能直接安装到 HotSpot 或已有应用中。VM 原有处理器、Guest 自定义 sigaction、非载体线程和完整切换窗口协议需要后续实现。

应用启动路径可能继承屏蔽测试通知的线程信号掩码（早期使用 SIGUSR1/SIGUSR2 时已观察到）。夹具记录 `inherited_blocked_signal_masks`，只在登记后解除两个测试通知信号的屏蔽，并在发送者停止、待处理测试信号排空后恢复原掩码，逐信号验证恢复结果。不会屏蔽或重放同步硬件故障。信号处理次数随调度变化，不作为性能指标。

### 普通应用身份测试

`android.py` 将已构建的 ARM64 bionic Host、Guest 和匹配 runtime 打包为独立 `dev.glibc.worker.domains` 应用，不申请额外权限。它核验制品哈希，再通过 Android CLI 安装启动。应用通过 `/system/bin/linker64` 启动原生测试子进程，保留应用 UID 和所继承的系统策略。**子进程内仍是同进程双 libc，但没有运行 ART 或 HotSpot；这不是 JNI 兼容测试。**

```sh
# 先用上述 run.py 构建；仅构建时省略 --serial。
python3 core/experiments/same-thread/android.py \
  --native core/build/experiments/same-thread/arm64-v8a-android-adopt-signals \
  --runtime "$DOMAIN_RUNTIME" --gradle "$GRADLE" \
  --sdk "$ANDROID_HOME" --serial "$ANDROID_SERIAL"
```

应用报告与成功摘要保存在 `core/build/experiments/same-thread/android-app-evidence/`。每轮启动前停止旧测试并删除旧报告，失败时不保留成功摘要。应用身份下曾观察到 24 个载体均继承了测试信号屏蔽状态，导致通知断言失败；显式处理掩码后，同一夹具通过。该结果只支持本节定义的应用身份原生路径，不能替代真实 JVM/JNI 回归。

### Host 信号所有权实验

HotSpot 夹具另外启用 `GW_DOMAIN_CHAIN`，在 VM 创建前安装 Host 注册路由。
该配置不同于上述由纯 C 测试直接拥有信号的配置。Guest runtime 的
`signal-guard.inc` 补丁在底层 `__libc_sigaction` 检查 Host 预留集合；
冲突注册和查询返回 `EBUSY`，不向 Guest 暴露 Host 处理器指针。
集合只能增加，不能在运行期间撤销。

Guest owner 在装载测试 DSO 前设置预留集合。此配置禁止
`GW_DOMAIN_PRELOAD`，因为预加载构造函数会早于该设置执行。
它尚不是可用于任意 Guest 可执行文件及其启动构造函数的接口。

`check-signals.py` 单独覆盖 Host 的处理器查询、掩码、errno、备用栈、
嵌套、一次性处理器、默认/忽略动作、系统调用中断/重启及并发替换。
注册时生成 W^X 的不可变代次入口，使实际内核掩码与对应处理器匹配；
SA_RESETHAND 只重置自身代次。代次入口暂保留至进程退出。
生产 JNI 实验使用 `signal-registry.inc` 的动态分页登记，配合 NPTL
入口/出口 hook 覆盖 Guest 自建线程，基础收养夹具仍保留固定测试参数。
完整边界和 HotSpot 复现步骤见 [JVM 实验文档](../../../jvm-jni/experiments/same-thread/README.md)。
独立双域信号虚拟化按 [todo.md](../../../todo.md) 的收益准入条件推迟。
