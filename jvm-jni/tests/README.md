# Android/JRE 集成测试

本工程在普通 Android 应用 `dev.glibc.worker` 中运行固定的 Android HotSpot，并启动同进程 GNU 线程域。测试目标是两套 libc 的数据边界、真实 JNI、装载和完整退出；Android Activity 使用的 ART 不作为本轮 JNI 桥兼容目标。

## 构建与运行

从工作区根目录执行。需要已发现的 SDK36、NDK、JDK17+、目标 GNU 编译器和显式 ADB 设备；SDK/NDK 使用环境变量或工作区 local.properties。首次构建下载并校验固定 JRE、SQLite、zstd 和 GNU 依赖。

```sh
GW_ABI=arm64-v8a jvm-jni/tests/android/gradlew -p jvm-jni/tests/android :app:assembleDebug
jvm-jni/tests/smoke/run.sh --device SERIAL
jvm-jni/tests/coverage/run.sh --device SERIAL
# 复用已构建 APK，仍校验 ABI 并安装。
jvm-jni/tests/coverage/run.sh --device SERIAL --reuse-apk
```

| 组别 | 通过范围 |
| --- | --- |
| smoke | 普通应用 UID、真实 JRE、同 PID/不同线程、POD/指针、malloc、errno/TLS、pthread、JNI 回调、Guest 退出、VM 恢复及销毁 |
| coverage | smoke，加 142 项 Java/JNI 断言、26 项装载契约、引用/异常/线程/重入/卸载/回滚、SQLite/zstd、11 项 JNI 名称回归与 49 项 ELF 元数据检查 |

smoke/coverage 都会执行 core 的 GNU mutex 夹具：普通锁/条件变量回归及 12 种 robust 属性组合的 ENOTSUP 拒绝，共 125 项检查；结构化结果必须存在且通过。旧 runtime 的 owner-death 失败复现、支持边界及独立运行方式见 [mutex 契约](../../core/docs/MUTEXES.md)。

当前默认 Zig 存储的 coverage 还运行三个 ClassLoader 周期：每轮 1,024 个不同 JNI 方法、320 个注册类、20,000 个 global 引用及描述符边界检查，退出后要求引用和 closure 为零。每轮另有 6 项失败/恢复检查：Java 捕获 Guest 深度超限、Guest 在上限处查询并清除异常、注册数量拒绝、失败状态码、线程上下文耗尽、释放后复用。`AUDIT_FAILURE_TEST` JSON 每轮明确记录 3 个预期桥错误；三轮原始错误计数必须恰为 9，多出或缺少均失败。

coverage 另有四个执行 scope 场景：自身关闭、A→B 关闭 A/B、A→B→A 关闭 B、异常展开后继续调用。`AUDIT_SCOPE_TEST` JSON 独立于原 142 项 Java 断言，要求四次 BUSY 拒绝、模块仍 LIVE、无额外资源释放或桥错误；后续真实卸载和最终零资源检查必须继续通过。C 层的线程归属和可转移 pin 对照在 bionic-dlopen CTest 内。

桌面故障注入入口为 `python3 android-jni/tests/failures.py`、`bindings.py`、`resources.py` 和 `audit.py`，分别覆盖生产 RPC 失败、libffi/注册、引用存储以及装卸载事务；它们使用 VM/worker 替身，不代表 Android 双 libc 验证。

严格模式默认使用解释器和 CheckJNI。超时、CheckJNI 违规、错误数据或生命周期不完整均为失败。覆盖为 235 个 JNI/VM 槽位清单及功能触达，正常运行不执行破坏性的 FatalError，不能描述为完整 JNI 规范或源代码分支覆盖。

`--output DIR` 指定证据目录，默认 `jvm-jni/build/test-results/<group>/`。保存报告、结构化核对结果、APK 散列和调度状态。每轮在设备/ABI 检查前取得目录独占锁，将旧 runner 制品移入 `history/<run_id>/`，原子写入 `run-state.json`。状态为 RUNNING、PASS、FAILED 或 INTERRUPTED；只有本轮全部检查通过才发布带相同 run_id 的 PASS summary。强制终止可能留下 RUNNING，不能将其当作通过；未知用户文件不会被归档。

安装后和 `--installed` 跳过安装前都核对手机上的 APK SHA256，记录为 `installed_apk_sha256`，不能复用未知版本。一个设备上按顺序运行，避免争用同一测试包和进程。`python3 jvm-jni/tests/scripts/test-evidence.py` 检查目录互斥、旧 PASS 隔离、设备发现失败及发布后失败的状态转换。

## 项目所有权

- 本项目 `android/`、`native/harness/`、`java/`、`scripts/`、`config/` 拥有真实 JRE 启动、应用入口、固定制品与集成编排。
- `android-jni/tests/` 拥有 JNI 行为、数组、SQLite 与多库夹具；构建时显式引用，不复制到本项目。
- core 的 POD 与 futex/诊断测试归 `core/tests/`，事件矩阵通过 core 的 CTest 单独执行。
- 纯 C 装载契约与 ELF 分类测试归 `bionic-dlopen/tests/`；ELF 测试只解析指定映像元数据，不执行目标 ARM 指令。
- 所有测试 JRE/JAR 下载归 `jvm-jni/vendor/`；生成源码、编译、APK 和结果归本项目 `build/`。Pojav 接入另在外部验证工程。

## 依赖库覆盖

```sh
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar lz4
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar snappy --reuse-apk
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar rocksdb --reuse-apk
```

首次选择会启用可选制品构建，随后可复用同 APK。固定散列、依赖闭包、原始映像策略和生命周期边界见 [兼容性手册](../docs/NATIVE_JAR_COMPATIBILITY.md)。额外 JAR 通过不代表所有使用它的模组通过。

## 计时与诊断

`--benchmark --benchmark-jit` 运行普通 JIT 的 scalar/callback 基准；`--payload-benchmark host|guest --benchmark-jit` 运行数据量基准。正式计时关闭 CheckJNI 和 RPC 诊断，严格语义检查单独运行。默认值仍为 16 contexts、20µs 主动等待、通知过滤和响应轮询关闭。

`--cpu-mask HEX` 仅对测量宿主及后续线程设置可用 CPU 亲和性，必须核对实际生效；`--array-copy-cache auto|on|off` 控制测试配置，on/auto 都保留大小门槛。产品运行时不读取测试标记。

同 APK 十对数据量 A/B 见 [数据量测量](../docs/PAYLOAD_LATENCY.md)，等待策略对照见 [RPC 测量](../docs/RPC_LATENCY.md)。数组语义、阈值和关闭方式见 [公开 JNI 数组约定](../../android-jni/docs/ARRAY_COPY_CACHE.md)。

原始延迟、完整性校验、热状态和生命周期必须一并保留。诊断 APK 与普通 APK 分开解读；库内计算、真实 JVM 工作、allocator 和调度不能都记为桥成本，微基准结果不能直接换算游戏收益。

## 安装包边界

`tests/scripts/check-sdk.py` 接受四个显式安装前缀、`--ndk` 与 `--abi`，复制到新的位置后编译/链接 C 和 C++ 静态 SDK 消费者。它检查导出包的相对依赖、附带的 libffi 和公开 C linkage，不启动 VM。输出默认在本项目 `build/sdk-check/`。

源码级多版本审计独立于真机测试，入口为 `scripts/audit-openjdk-loader.py`；固定输入与结论使用方式见 [JRE 兼容性契约](../docs/JRE_COMPATIBILITY.md)。源码形态相同不计作运行通过。

## 真实桌面 VM 的装载适配测试

需要 Linux x86_64、C 编译器及 JDK17+。以下命令下载或复用 SHA256 固定的 Temurin JRE8/21/25；本机 11u、17u 的目录由调用方显式提供，不能假设示例路径存在：

```sh
python3 jvm-jni/tests/scripts/test-adapters.py --pinned \
  --runtime 3:/path/to/reviewed-jre11u \
  --runtime 5:/path/to/reviewed-jre17u
# 也可以只测一个审阅后的 VM，不下载其他版本：
python3 jvm-jni/tests/scripts/test-adapters.py --runtime 6:/path/to/reviewed-jre25
```

`--runtime FAMILY:HOME` 的数字对应 `gw_jvm_loader_family`。不能把 11 GA 的族 2 与带回移植参数的 11u 族 3 混用，也不能把旧 Android JRE17 的族 4 与本轮本机 17u 的族 5 混用。接口族选择须审阅，不由测试脚本猜测。

本测试链接生产适配器及 C loader，在真实 VM 内通过 System.load 触发钩子，但测试 transport 直接调用主机 dlopen；它不创建双 libc。每个 VM 的普通进程验证 27 项条件，包括制品/族拒绝、部分注册失败的恢复、JNI_OnLoad 回调和拒绝、普通库回退、符号重载、不同 ClassLoader 所有权、两轮回收/自动卸载/再加载及原钩子恢复。另开一个 VM 进程注入注册和恢复同时失败，验证 3 项丢弃进程条件，不复用该进程。

故障注入只在测试中临时替换当前线程 JNI 表的 RegisterNatives，其余函数和 CheckJNI 保持原样；生产适配器没有此开关。每次运行先启动同参数、未加载适配器的空白程序作为启动对照。部分 JRE8 launcher 会在进入 main 前按参数数目产生 JNI 告警，必须与该对照逐字匹配；main 之后的任何 JNI 告警都判失败。ClassLoader 回收使用 GC 和有界等待，不通过 System.runFinalization 引入该旧 VM 的额外告警。

输出在 `jvm-jni/build/adapter-tests/`，包括编译输入、每个 VM 的散列、JSON 断言与原始日志；不通过打印措辞判定普通断言。Android 覆盖另外输出 JNI 能力协商的结构化结果，确认当前版本可用和未来版本拒绝。
