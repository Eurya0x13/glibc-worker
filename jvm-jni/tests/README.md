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

当前默认 Zig 存储的 coverage 运行三个 ClassLoader 周期：每轮 1,024 个不同 JNI 方法、320 个注册类、20,000 个 global 引用及描述符边界检查。每轮另有重入/注册状态/上下文复用检查；32 个 Host 线程保持绑定时，另有 32 个 Guest 来源线程同时附着，且 32 层重入超过初始 12 层和第一扩展页。`DYNAMIC_CAPACITY_TEST` 必须逐轮存在并证明实际容量增长。`AUDIT_FAILURE_TEST` 逐轮要求零桥错误，最终引用和 closure 为零；分配失败由桌面故障注入覆盖，不能再将超过初始容量当作预期失败。

`AUDIT_OWNERSHIP_TEST` 验证共享依赖函数在已知模块上下文中注册、后台顶层函数逆向归属、无上下文的共享依赖/混合批次拒绝、显式注销交接，以及卸载旧所有者不影响新所有者、最终所有者卸载后入口未绑定。

coverage 另有四个执行 scope 场景：自身关闭、A→B 关闭 A/B、A→B→A 关闭 B、异常展开后继续调用。`AUDIT_SCOPE_TEST` JSON 独立于原 142 项 Java 断言，要求四次 BUSY 拒绝、模块仍 LIVE、无额外资源释放或桥错误；后续真实卸载和最终零资源检查必须继续通过。C 层的线程归属和可转移 pin 对照在 bionic-dlopen CTest 内。

`AUDIT_CLOSE_WAIT_TEST` 要求两个真实 JNI 调用同时回调 Java 并关闭对方模块，均返回 BUSY；两次 close 返回前，两个 native 调用都保持在途。测试核对模块仍 LIVE、无提前释放且随后可继续调用。barrier/join 的测试期限不代表生产 bridge 具有超时取消能力。两/三线程的生产 C loader 对照、目标 pin 和探测后竞态在 bionic-dlopen 的 `close_waits` 内。

桌面故障注入入口为 `python3 android-jni/tests/failures.py`、`bindings.py`、`resources.py` 、`audit.py` 和 `context_pages.py`，分别覆盖生产 RPC 失败、libffi/注册、引用存储、装卸载事务以及共享页扩容/并发领取；它们使用 VM/worker 替身，不代表 Android 双 libc 验证。

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

`--benchmark --benchmark-jit` 运行普通 JIT 的 scalar/callback 基准；`--payload-benchmark host|guest --benchmark-jit` 运行数据量基准。正式计时关闭 CheckJNI 和 RPC 诊断，严格语义检查单独运行。默认初始容量为 16 contexts、20µs 主动等待、通知过滤和响应轮询关闭。

`--cpu-mask HEX` 仅对测量宿主及后续线程设置可用 CPU 亲和性，必须核对实际生效；`--array-copy-cache auto|on|off` 控制测试配置，on/auto 都保留大小门槛。产品运行时不读取测试标记。

同 APK 十对数据量 A/B 见 [数据量测量](../docs/PAYLOAD_LATENCY.md)，等待策略对照见 [RPC 测量](../docs/RPC_LATENCY.md)。数组语义、阈值和关闭方式见 [公开 JNI 数组约定](../../android-jni/docs/ARRAY_COPY_CACHE.md)。

原始延迟、完整性校验、热状态和生命周期必须一并保留。诊断 APK 与普通 APK 分开解读；库内计算、真实 JVM 工作、allocator 和调度不能都记为桥成本，微基准结果不能直接换算游戏收益。

跨修订评审可用 `coverage/review_changes.py`：`--baseline` / `--candidate` 指定保留的 ARM64 APK，`--baseline-ref` / `--candidate-ref` 指定对应功能检查器的 Git 修订，`--device` 和 `--output` 指定设备与全新结果目录。默认十对平衡顺序、普通 JIT，每轮新进程。脚本核对 benchmark 类、Guest 基准 DSO 和固定 JRE 的散列；全部轮次通过各自的功能检查后才汇总，并在最后恢复候选包做严格冒烟。

检查器修订不等于 APK 构建来源证明。跨修订的覆盖夹具、启动代码、GNU runtime 和预热状态可能不同，这类结果只能描述保留制品的整体表现，不能单独归因于某个提交。当前 scalar/callback 基准使用缓存的 methodID，且计时调用来自新建的 Host 工作线程；它不覆盖重复 ID 查找、owner 上下文的控制扫描或深层重入成本。

已保留的 ARM64 十对结果见 [runtime-performance-arm64.json](results/runtime-performance-arm64.json)，引用存储组件的独立 CPU 测量见 [reference-cost-arm64.json](../../android-jni/tests/results/reference-cost-arm64.json)。后者使用最小 VM 替身，不能当作真实 JNI 的完整延迟。

ID 全池扫描替换为专用索引后的组件 A/B、OOM/类身份回归，以及固定 JRE17 严格覆盖和冒烟记录见 [id-index-arm64.json](../../android-jni/tests/results/id-index-arm64.json)；该结果不更新或替代此前 owner 扫描及 8 线程往返的待定位信号。

## 安装包边界

`tests/scripts/check-sdk.py` 接受四个显式安装前缀、`--ndk` 与 `--abi`，复制到新的位置后编译/链接 C 和 C++ 静态 SDK 消费者。它检查导出包的相对依赖、附带的 libffi 和公开 C linkage，不启动 VM。输出默认在本项目 `build/sdk-check/`。

源码级多版本审计独立于真机测试，入口为 `scripts/audit-openjdk-loader.py`；固定输入与结论使用方式见 [JRE 兼容性契约](../docs/JRE_COMPATIBILITY.md)。源码形态相同不计作运行通过。

## 真实桌面 VM 的装载适配测试

需要 Linux x86_64、C 编译器及 JDK17+。桌面契约测试使用普通 JIT 和 CheckJNI；双散列重复校验不在解释器内进行性能测量。以下命令下载或复用 SHA256 固定的 Temurin JRE8/21/25；本机 11u、17u 的目录由调用方显式提供，不能假设示例路径存在：

```sh
python3 jvm-jni/tests/scripts/test-adapters.py --pinned \
  --runtime 3:/path/to/reviewed-jre11u \
  --runtime 5:/path/to/reviewed-jre17u
# 也可以只测一个审阅后的 VM，不下载其他版本：
python3 jvm-jni/tests/scripts/test-adapters.py --runtime 6:/path/to/reviewed-jre25
```

`--runtime FAMILY:HOME` 的数字对应 `gw_jvm_loader_family`。不能把 11 GA 的族 2 与带回移植参数的 11u 族 3 混用，也不能把旧 Android JRE17 的族 4 与本轮本机 17u 的族 5 混用。接口族选择须审阅，不由测试脚本猜测。

本测试链接生产适配器及 C loader，在真实 VM 内通过 System.load 触发钩子，但测试 transport 直接调用主机 dlopen；它不创建双 libc。每个 VM 的普通进程验证 31 项条件，包括制品/族拒绝、部分注册失败的恢复、JNI_OnLoad 回调和拒绝、普通库回退、符号重载、不同 ClassLoader 所有权、两轮回收/自动卸载/再加载及原钩子恢复。另开一个 VM 进程注入注册和恢复同时失败，验证 3 项丢弃进程条件，不复用该进程。

故障注入只在测试中临时替换当前线程 JNI 表的 RegisterNatives，其余函数和 CheckJNI 保持原样；生产适配器没有此开关。每次运行先启动同参数、未加载适配器的空白程序作为启动对照。部分 JRE8 launcher 会在进入 main 前按参数数目产生 JNI 告警，必须与该对照逐字匹配；main 之后的任何 JNI 告警都判失败。ClassLoader 回收使用 GC 和有界等待，不通过 System.runFinalization 引入该旧 VM 的额外告警。

输出在 `jvm-jni/build/adapter-tests/`，包括编译输入、每个 VM 的散列、JSON 断言与原始日志；不通过打印措辞判定普通断言。Android 覆盖另外输出 JNI 能力协商的结构化结果，确认当前版本可用和未来版本拒绝。

## 单 Runtime 生命周期

smoke/coverage 必须各输出唯一且通过的 `RUNTIME_LIFECYCLE_TEST`：一进程一次启动、错误线程关闭拒绝、正常 STOPPED、重复关闭和重启拒绝。后续 Guest 线程退出及 VM 销毁仍独立验证。桌面 `android-jni/tests/lifecycle.py` 覆盖初始化/关闭故障和资源保留；`audit.py` 覆盖隔离后在途调用保留、迟到响应及由外部测试父进程处置永久停滞子进程。见 [生命周期契约](../../android-jni/docs/LIFECYCLE.md)。

## 多 Android JRE 制品

默认使用固定 Pojav17；其他候选必须显式选择 ID。提取缓存和安装结果都检查 libjava/libjvm 配对；未知、混用、架构不符和摘要不符均拒绝。JRE25 额外固定测试用 NDK libc++_shared 散列，不从任意搜索路径补库；需要匹配清单的 NDK 支持文件。

```sh
python3 jvm-jni/tests/scripts/test-jre-artifacts.py
python3 jvm-jni/tests/scripts/jre_artifacts.py --abi arm64-v8a --home /path/to/jre
GW_DEPS_READY=1 python3 jvm-jni/tests/scripts/test-android-jres.py \
  --device SERIAL --artifact angelaura-8-arm64 \
  --artifact angelaura-21-arm64 --artifact angelaura-25-arm64 \
  --artifact pojav-17-ec28559-arm64-v8a
```

首次构建依赖未准备时省略 `GW_DEPS_READY=1`。矩阵先构建并固定全部 APK，再在同一设备顺序安装，保留每个 APK、构建 manifest、散列和运行报告；smoke 通过才继续 coverage。任一失败使整个命令失败，启动受阻单独标注，不能用“识别成功”代替运行验证。单项也可设置 `GW_JRE_ARTIFACT=ID` 后使用上面的构建/运行入口。支持边界见 [JRE 契约](../docs/JRE_COMPATIBILITY.md)。

模拟器使用 `--abi x86_64 --device emulator-5554 --emulator AVD_NAME`，并选择清单中的 x86_64 制品 ID。此模式在构建前关闭指定 AVD，构建结束后停止 Gradle，确认进程退出后才启动 AVD；运行结束关闭 AVD。真机模式不自动管理其他模拟器，执行前应先关闭它们。

AngelAura 测试夹具按原始 JRE 目录部署只读库，复用 Pojav 的预加载与 `JLI_Launch` 路径。来源 commit 与文件散列见 `config/pojav-startup.json`，适配代码只属于测试应用。部署后分别尝试直接运行 `bin/java -version` 和 `/system/bin/linker64 bin/java -version`，记录 `JRE_EXEC_PROBE`；代理探针是子进程，真正的桥测试在应用进程内运行的 HotSpot 中执行。

ARM64 应用通过 manifest 关闭堆指针标记。exec 子进程不继承该声明，因此其环境额外预加载测试库 `libjre_exec_compat.so`，在 Android 12 及以上使用 bionic `mallopt` 关闭本子进程的堆标记；不修改系统设置，也不向 ART 或 glibc Guest 预加载该库。未设置时，固定 JRE8 的独立版本查询可在 `assembler_aarch64.hpp` 触发 `Field too big for insn`，即使不加载任何桥也会失败。此兼容设置是测试启动条件，不属于 core 或装载适配器 API。

JRE8 额外以相同参数启动不加载桥的 `JreBaseline`。进入 main 前的 CheckJNI 告警必须与独立对照逐行匹配，main 后的告警一律失败，致命 JNI 诊断也不能由对照豁免。`scripts/test-jre-report.py --group coverage REPORT` 对制品、执行探针和存在的启动对照执行破坏性输入验证；它只修改报告副本。

固定 JRE8 的严格 coverage 当前会在 VM 的 CheckJNI 数组提交路径失败；矩阵如包含它，整体仍返回失败。独立 JNI 最小复现不加载 glibc-worker：

```sh
python3 jvm-jni/tests/scripts/test-array-commit.py --device SERIAL
```

该命令使用已安装测试 App 的 UID，部署独立的固定 JRE8 与最小 JNI 库，验证不开启 CheckJNI 时回写成功、开启后退出 134。它的 PASS 表示复现了已知 VM 缺陷，结果包含 `strict_jni_supported=false`；正常/严格日志分开保存，随后清理本次手机临时目录。

正常 JIT 模式的完整 JRE8 回归可复用保留的 ARM64 JRE8 APK：

```sh
GW_JRE_ARTIFACT=angelaura-8-arm64 jvm-jni/tests/coverage/run.sh \
  --device SERIAL --reuse-apk --apk /path/to/retained-jre8.apk \
  --benchmark --benchmark-jit --output jvm-jni/build/jre8-normal
```

此入口保留全部功能覆盖，同时执行既有的附加基准。它不是严格组的替代品，也不能凭单次结果下性能结论。制品支持模式见 [兼容性契约](../docs/JRE_COMPATIBILITY.md)。

已保留的 ARM64 制品/模式结果、APK 与原始报告摘要、独立 VM 反例及校验器反例见 [android-jres-arm64.json](results/android-jres-arm64.json)。其中严格矩阵整体为 FAIL，原因是上述 JRE8 VM 缺陷；每项结果和正常模式证据独立列出。新的运行由 runner 生成新的 run_id，不能沿用这份记录作为新制品通过证明。
