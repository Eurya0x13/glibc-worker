# Production JNI paths with same-thread domain switching

本测试让真实 Guest JNI 桥分别通过 worker 通知和同线程栈/TLS 切换执行，验证兼容性并比较相同 JNI 工作的耗时。正式启动接口见 [JNI runtime 接入](../../docs/zh/RUNTIME.md)。

## 接入边界

两种模式使用同一个 Host、Guest、DSO、Java 测试类和打过收养补丁的 glibc；当前接入构建在未设置 `GW_DOMAIN_MODE` 时默认同线程；显式 `GW_DOMAIN_MODE=worker` 选择线程后端，`GW_DOMAIN_MODE=domain` 显式选择同线程。空值和其他名称均拒绝启动，不静默改用其他后端。APK 兼容性入口不再强制写入该变量，而是验证原生默认值；A/B runner 继续为每轮显式选择后端。

[默认后端回归证据](../results/same-thread/default-backend/README.md) 覆盖缺省、显式选择、非法选择与 ART/HotSpot 同进程装卸载。

- 复用 `full_host_start`、`full_guest_start`、Host controller、公开 loader API，以及真实的 `JNI_OnLoad`、`RegisterNatives`、`JNI_OnUnload` 和关闭流程。
- 复用生产生成的 JNI 表、参数封送、引用/ID 存储、Zig 绑定存储和 libffi 跳板。没有用测试专用 JNI 函数表代替生产桥。
- `command.c` / `rpc.c` 的可选 hook 改变请求发布后的等待方式；`full_host.c` / `guest_loader.c` 的 hook 在同线程模式跳过无需发送的对端通知。未定义 `GW_DOMAIN_JNI` 时保持原有路径。
- Host 进入 Guest 时换栈和 TLS。Guest JNI 回调先切回原 Host 栈/TLS，再调用该载体所属的真实 `JNIEnv`。嵌套 native 调用仍使用生产深度槽和 dispatcher。
- `fh_context` 在真实首次 native 调用时申请上下文；同线程扩展在此分配栈/TCB，使用原 pthread-key 析构路径回收。Java 创建的线程不再由夹具预绑定槽位。上下文和重入沿用生产分块扩容；信号登记也改为稳定分页存储，可复用记录。
- Guest 自建 pthread 通过附着 helper 回调 Java，支持再次进入 native。补丁 glibc 在 NPTL 线程入口和出口登记 Guest TLS。
- 部分初始化失败会撤销信号登记、释放栈并归还上下文；注入 TCB 分配失败后，在同一个 Java 线程重试成功。Guest pthread 信号登记本身若因缺内存失败则终止隔离进程，不能放行未登记的 Guest 线程。

此夹具直接构建 JNI 桥源文件并使用自己的启动参数。已安装包的后端选择和生命周期由
独立的 [SDK 消费者测试](../../docs/zh/SDK_TESTS.md) 验证。

## 兼容性断言

每次 runner 都先验证未指定后端的八线程同线程执行及空值／非法选择拒绝，再运行 worker/domain × 1/8 线程矩阵，开启 CheckJNI；任一结果错误或 CheckJNI/信号处理器警告导致失败。复用 `guest_conformance.c` 和生成的方法/字段/数组矩阵，要求正常进程命中 234 个接口入口，再用两种后端各自的隔离子进程验证 FatalError 确实触发 SIGABRT，覆盖 230 个 JNI 与 5 个 VM 入口。Guest DestroyJavaVM 等明确拒绝的语义也计入入口覆盖，不代表这些操作被允许。随后性能模式关闭 CheckJNI 和编译日志。

| 切面 | 断言 |
| --- | --- |
| 装载生命周期 | loader open/close 成功，OnLoad 注册 native 方法，OnUnload 恰好回调一次，除预期注入错误外无桥错误，VM 销毁及信号恢复成功 |
| 标量及回调 | 逐值校验，Guest 调用 Java 后正确返回 |
| 重入 | Java → Guest → Java → Guest，32 层重入，并校验 Java ThreadLocal |
| 引用 | local/global/weak、局部帧和对象身份保持 |
| 字符串 | modified UTF-8 往返，包括中文、NUL 和代理对 |
| 数组 | 128 B / 64 KiB 元素访问、释放及求和校验 |
| 异常 | Guest ThrowNew，Java 捕获正确类型和消息，随后继续调用 |
| VM 代理 | GetJavaVM/GetEnv、版本拒绝；32 个 Guest pthread，普通/daemon 附着、回调重入、detach 和 EDETACHED |
| 线程生命周期 | 三轮各 32 个同时存活的 Java 上下文、TLS 初值和复用、共享 monitor 合计 960 次更新；创建/回收计数一致 |
| 失败恢复 | TCB 分配故障注入返回 OutOfMemoryError；同线程再次调用成功，无半初始化上下文残留 |
| VM 协作 | native 阻塞期间 20 轮栈采样/GC；JFR 开始、采样、结束成功，VM SR 信号转发计数非零 |

装载仍通过显式生命周期测试。没有测试这套后端的 ClassLoader GC 自动卸载、所有 JRE 制品或真实模组。本目录基础矩阵通过公开 loader API 发起。私有 `System.load` 钩子的独立接入、ClassLoader 自动卸载及晚加载信号反例见 [JVM 装载集成](../../../jvm-jni/docs/zh/SYSTEM_LOAD.md)。

信号合同继承 [HotSpot 夹具](../../../jvm-jni/tests/same-thread/README.md)：Host 保留 VM 必需信号，Guest 冲突注册/查询返回 `EBUSY`。每次 Host 注册生成独立的只读可执行入口，绑定该代处理器；并发替换不再把旧内核掩码和新处理器混用，SA_RESETHAND 也只重置自身代次。独立信号合同测试覆盖 1,000 次并发替换，并暂停在内核已重置 SA_RESETHAND、路由尚未执行的窗口，验证查询、替换返回的旧处理器及延迟到达的旧代次不会覆盖新注册。

JFR 提供真实 VM 采样负载；[OpenJDK 的 Java 采样路径使用 SuspendedThreadTask，native 采样则有独立路径](https://github.com/openjdk/jdk17u/blob/master/src/hotspot/share/jfr/periodic/sampling/jfrThreadSampler.cpp)。本测试断言实际 SIGUSR2 转发和 VM 正常完成，不把非零计数解释成每次中断都发生在 Guest TLS 下。独立 HotSpot 夹具另外覆盖 JIT 同步故障恢复和线程析构；这些压力流量均不加入性能计时。

本基础夹具覆盖首版的信号保留策略。当前安装 SDK 的独立 Guest 投递、在线回收及跨域恢复契约见
[core 信号与域切换](../../../core/docs/zh/DOMAINS.md) 和
[SDK 测试](../../docs/zh/SDK_TESTS.md)。

## 构建和运行

从仓库根目录运行。先按 [core 测试说明](../../../core/tests/same-thread/README.md) 构建带收养和信号所有权补丁的 runtime，并准备两种 libc 对应的 libffi、Zig 工具链及指定 Android JRE 17 制品。

```sh
# 桌面 x86_64：Host 与 Guest 是两套 glibc，不能替代 bionic 验证。
python3 android-jni/tests/same-thread/build.py \
  --abi x86_64 --ndk "$ANDROID_NDK" --jdk "$JDK17" \
  --runtime "$DOMAIN_RUNTIME_X86_64"
python3 android-jni/tests/same-thread/run.py \
  --build android-jni/build/tests/same-thread/x86_64 --jdk "$JDK17"

# 真机 ARM64：bionic Host + Android HotSpot + glibc Guest。
python3 android-jni/tests/same-thread/build.py \
  --abi arm64-v8a --ndk "$ANDROID_NDK" --jdk "$JDK17" \
  --runtime "$DOMAIN_RUNTIME_ARM64"
python3 android-jni/tests/same-thread/run.py \
  --build android-jni/build/tests/same-thread/arm64-v8a \
  --jdk "$JDK17" --serial "$ANDROID_SERIAL" --pairs 10 --threads 1
python3 android-jni/tests/same-thread/run.py \
  --build android-jni/build/tests/same-thread/arm64-v8a \
  --jdk "$JDK17" --serial "$ANDROID_SERIAL" --pairs 10 --threads 8
```

Android 构建核对 `jvm-jni/build/tests/android/assets/android-jre/jre-artifact.json`、对应 `jniLibs/arm64-v8a` 和 `jvm-jni/build/native/jre-layout.h` 的制品绑定。共享的 `vm-start.c` 只负责实验 VM 启动，不引入版本自动装载。

应用权限验证：

```sh
python3 jvm-jni/tests/same-thread/android.py --jni-domain \
  --native android-jni/build/tests/same-thread/arm64-v8a \
  --runtime "$DOMAIN_RUNTIME_ARM64" \
  --jre-assets jvm-jni/build/tests/android/assets/android-jre \
  --jre-libs jvm-jni/build/tests/android/jniLibs/arm64-v8a \
  --gradle "$GRADLE" --sdk "$ANDROID_HOME" --serial "$ANDROID_SERIAL"
```

执行子进程复用 `jre_exec_compat.c` 在 VM 启动前关闭不兼容旧 Android HotSpot 的堆指针标记；该设置不进入 ART 或 Guest。仅设置 APK manifest 不能覆盖 exec 后的进程。

APK `dev.glibc.worker.domains.hotspot` 在应用身份下通过 linker64 启动隔离原生子进程。bionic、HotSpot 与 glibc 同处该子进程，**不与 Activity 的 ART 同处一个进程**。应用验证固定使用八载体 domain 模式，不做性能计时。

## 测量方法及含义

每组十对，交替 AB/BA，每次新建 VM 进程。每类先执行八批预热，再记录三十批：普通批 256 次，64 KiB 数组批 64 次。校验每对各线程各类的输出 checksum 相同。五类依次执行；多线程只在整个测试的开始/结束同步，各类之间不设屏障，所以八线程结果是并发混合工作流，不能视为严格锁步的单类负载。

每个进程先取 `batch_ns / iterations` 的中位数，八线程时合并各线程批次；再汇总十对。`paired_ratio_median` 是逐对 domain/worker 比值的中位数，不等于两个汇总中位数的比值。这些是**批均摊调用耗时，不是单次调用 P50/P99，更不是裸切换耗时**。

两侧都包含相同信号注册层、相同 patched glibc 和相同生产 JNI 开销。worker 使用现有 event/futex 路径，`GW_RPC_SPIN_NS=20000`，profile/ready-poll/wake-filter 关闭。VM 使用 SerialGC、16–128 MiB 堆、ActiveProcessorCount=2、同步 JIT 编译。没有绑核；没有控制温度、DVFS 或后台负载，不能从本次结果归因各项调度成本。数组计时还包含元素访问及求和，nested4 包含多次跨域往返。

这组计时覆盖夹具内相同 JNI 工作的两种交接方式。需要普通 bionic JNI 参考时，使用
[同进程性能测量](../../../jvm-jni/docs/zh/PERFORMANCE.md)。

原始输出、manifest、逐批样本、逐对 checksum 和汇总保存在本项目 `build/tests/same-thread/<abi>/runs/<timestamp>/`。APK 报告在 `jvm-jni/build/tests/same-thread/android-app-evidence/`。最新兼容性和复测结果见 [compatibility](../results/same-thread/compatibility/README.md)；首次受限夹具的测量保留在 [results](../results/same-thread/README.md)，不是当前实现的性能证据。
