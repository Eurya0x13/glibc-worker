# HotSpot same-thread domain fixture

本测试复用 [core 的收养夹具](../../../core/tests/same-thread/README.md)，在 Host 域启动真实 HotSpot，验证同线程 Guest 调用、信号和线程生命周期。Java/native 入口与 APK 由测试脚本构建。

生产 Guest `JavaVM/JNIEnv` 代理表和交替 A/B 由 [android-jni 同线程测试](../../../android-jni/tests/same-thread/README.md) 覆盖。它复用这里的 `vm-start.c` 和 APK supervisor。`android.py --jni-domain` 选择 JNI 测试的结构化校验规则。

## 执行路径和断言

每批创建 8 个 Host pthread，共 3 批。线程附着到 HotSpot 后进入自己的 glibc 域；Guest 请求回调时切回 Host 栈/TLS，再调用真实 `JNIEnv` 执行 Java。Java 用 `RegisterNatives` 注册的 `reenter()` 再次进入 Guest，最多八层。`JNIEnv` 只在所属 Host 线程和 Host 域使用。

- 19,200 次 Guest 调用和 Java 回调，16,800 次 Java native 重入；逐次检查数据、深度、TID、独立栈/TLS、errno 与浮点状态。
- Java 局部对象跨重入保持存活，返回后检查值和对象身份；后台 Java 线程反复触发 GC。
- 每次 Java 回调产生并捕获预期的空指针异常。启用 `-Xcheck:jni`、JIT 和编译日志，runner 必须观察到 `DomainProbe::callback` 编译及非零故障转交计数。
- 24 次 `AttachCurrentThread/DetachCurrentThread`，Guest pthread key/TLS 析构、cleanup handler 和 Host join；12 次 Guest 最外层 `pthread_exit` 不终止载体。
- 最后注销测试 native 方法、释放类全局引用、停止 GC 线程并成功 `DestroyJavaVM`。

Java GC 次数和信号次数随调度变化，仅用作覆盖证据。夹具带有 CheckJNI、强制编译与主动 GC，不能用运行时间推断生产性能。

## 信号边界

core 提供可选的 `GW_DOMAIN_CHAIN` Host 注册层，在创建 VM 前安装。Host 的 `sigaction()`、`signal()` 和 ARM64 Android 的 `sigaction64()` 进入该层；查询返回 VM 注册的逻辑处理器，内核入口则先恢复 Host TP/x18，再调用相应处理器，最后恢复被中断域。保留原始 ucontext，只有夹具自有的已知 fault 会修改 PC。CheckJNI 保持开启；runner 将处理器被修改的警告视为失败，并在结束时核对实际内核处理器。

首版采用 **Host 保留 VM 必需信号所有权** 的合同。独立构建的 Guest glibc 在 `__libc_sigaction` 拒绝这些信号的注册和查询，返回 `EBUSY`，覆盖 `signal()` 等 libc 内部入口。查询失败避免把 Host 处理器当作 Guest 函数返回；预留只能增加，不能撤销。每个收养载体验证全部预留信号的注册、`signal()` 和查询均被拒绝。

测试使用 Linux 实时信号 50/51，在安装前检查未被占用，普通线程原有掩码在退出时恢复。另从 Guest 域主动发送 SIGUSR2、SIGPIPE、SIGXFSZ，共 72 次；逐个核对实际转交次数。Host 已忽略的信号保留忽略语义，不计入转交次数；夹具在载体登记后临时解除这三个测试信号的屏蔽，结束时逐项验证继承掩码已经恢复；应用子进程可能继承屏蔽的 SIGPIPE。这个测试设置不代表运行时接管 Guest 信号掩码。SIGUSR2 在这个基础夹具中覆盖空闲的 HotSpot SR 处理器入口；生产 JNI 夹具另用 JFR 触发真实 VM 采样和 SR 投递，结果见其兼容性报告。自定义 `_JAVA_SR_SIGNUM` 明确拒绝启动，避免绕开预留合同。

当前边界：

- Guest owner 和夹具创建的后台 pthread 使用独立登记；它们收到需转交的信号时终止测试，不能携带 Guest TLS 调用 VM。owner 启动到登记前的窗口仍未覆盖。
- 已登记载体的未知 Guest 硬件故障直接终止隔离测试，不让 VM 解释 Guest 栈。
- Guest 直接执行信号系统调用、显式绕过注册层、修改 VM 所需信号掩码、任意 Guest 自定义处理器投递均不在支持范围内。此配置也禁止 `GW_DOMAIN_PRELOAD`；应用用标准 SONAME 符号链接解析运行库，所有测试 DSO 均在所有权设置后装载。
- Host 注册记录及可执行入口按隔离进程生命周期保留。每次注册绑定不可变代次，内核掩码与处理器一致；一次性处理器只重置自身代次。独立测试覆盖 1,000 次并发替换、SA_RESETHAND、掩码、备用栈和重启语义。通用回收、跨域 unwind/longjmp 和任意 VM suspension 组合仍未覆盖。
- Guest 嵌套回调中退出、完整 Guest runtime 卸载尚未覆盖。

本基础夹具验证 Host 保留策略。现行 Guest 投递与跨域恢复契约见
[core 信号与域切换](../../../core/docs/zh/DOMAINS.md)。

Host 信号合同可独立复现：

```sh
python3 core/tests/same-thread/check-signals.py
python3 core/tests/same-thread/check-signals.py \
  --ndk "$ANDROID_NDK" --serial "$ANDROID_SERIAL"
```

这里验证的是有限的线程收养、Java 重入、GC 和同步故障恢复切面，不是完整 JNI 兼容矩阵。

## 构建和运行

从仓库根目录运行。先按 core 测试文档准备匹配的收养 runtime。

```sh
# 桌面 Linux：Host 与 Guest 是两套 glibc。
python3 jvm-jni/tests/same-thread/run.py \
  --jdk "$JDK17" --runtime "$DOMAIN_RUNTIME_X86_64"

# 真机 shell：bionic HotSpot + glibc Guest。
python3 jvm-jni/tests/same-thread/run.py \
  --jdk "$JDK17" --runtime "$DOMAIN_RUNTIME_ARM64" --ndk "$ANDROID_NDK" \
  --jre-assets jvm-jni/build/tests/android/assets/android-jre \
  --jre-libs jvm-jni/build/tests/android/jniLibs/arm64-v8a \
  --jre-profile jvm-jni/build/native/jre-layout.h \
  --serial "$ANDROID_SERIAL"
```

`--build-only` 只构建，不部署执行。Android 启动复用现有 `jre_layout_spike.c` 的制品绑定路径适配：runner 核对 libjvm、libjava 的 SHA256 与 profile，只接受已准备的 Android JRE 17。没有新增版本自动识别，也不将 JRE 17 的结果推广到 8/21/25。

```sh
# 独立 APK：不修改现有启动器或 glibc-worker 测试应用。
python3 jvm-jni/tests/same-thread/android.py \
  --native jvm-jni/build/tests/same-thread/android \
  --runtime "$DOMAIN_RUNTIME_ARM64" \
  --jre-assets jvm-jni/build/tests/android/assets/android-jre \
  --jre-libs jvm-jni/build/tests/android/jniLibs/arm64-v8a \
  --gradle "$GRADLE" --sdk "$ANDROID_HOME" --serial "$ANDROID_SERIAL"
```

APK 包名为 `dev.glibc.worker.domains.hotspot`。Activity 只负责解包 JRE 数据、将 JRE 预期的库路径链接到 Android 原生库目录，并通过 linker64 启动隔离子进程。**HotSpot、bionic 和 glibc 位于同一个原生子进程中，具有应用 UID；它没有与父进程 ART 共存。** 测试限时 120 秒，失败/超时保存日志并丢弃子进程，不回退到现有 worker 冒充通过。

输出位于本项目 `build/tests/same-thread/`：`linux/`、`android/` 保存构建 manifest、stdout/stderr 和成功 summary；`android-app-evidence/` 保存应用报告和 APK 哈希。失败时清除上一轮成功摘要。

已观察到的通过平台为桌面 OpenJDK 17、Android ARM64 JRE 17 shell 和 V2239A Android 13 应用身份。报告中的 `full_guest_jni_bridge_validated`、`production_ready` 和 `performance_measured` 均保持 `false`。

## 动态线程登记

生产 JNI 夹具使用稳定分页的信号登记表，线程退出后复用记录，页保留到进程退出。Host 在上下文创建/析构时登记和撤销；Guest NPTL 在线程入口和 TLS 析构后调用不依赖 Host TLS 的登记钩子。Guest 自建线程继续原反向 JNI 路径；它们收到需要 Host 处理的信号会明确终止隔离进程，不能假装存在对应的 Host TLS。上述八载体基础夹具仍保留自己的固定测试参数。

Android exec 子进程复用 `jre_exec_compat.c`，在启动旧 HotSpot 前关闭其不支持的堆指针标记；仅设置应用 manifest 不能覆盖 exec。该兼容设置不修改 Activity 的 ART 进程。

## Production System.load integration

真实装载钩子、ClassLoader 自动卸载与应用身份验证现有独立入口，见 [SYSTEM_LOAD.md](../../docs/zh/SYSTEM_LOAD.md)。该接入通过不代表 Pojav 晚加载的信号路由已经可用；文档同时保留真机最小反例。


## Explicit late loading and ART coexistence

[LATE_SIGNALS.md](../../docs/zh/LATE_SIGNALS.md) describes the `--late-signal` build and
`--inline-vm` APK mode. Unlike the original exec-child fixture above, inline mode
keeps ART, HotSpot and both libc domains in the same process. It requires an
explicitly reviewed ART signal profile; it is not generic late ELF interposition.

生产装载与 ART 共存后的收益测试见 [PERFORMANCE.md](../../docs/zh/PERFORMANCE.md)。它比较同一 APK 中的 worker、同线程后端及普通 bionic JNI 参考。
