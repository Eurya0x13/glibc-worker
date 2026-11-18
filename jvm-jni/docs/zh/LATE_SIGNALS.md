# 显式晚接入信号链

[English](../en/LATE_SIGNALS.md) | [简体中文](../zh/LATE_SIGNALS.md)

已审阅的信号 profile 支持在 HotSpot 启动后、应用开始 JNI 工作前装载桥。应用通过
[运行时启动接口](../../../android-jni/docs/zh/RUNTIME.md) 选择后端。回归夹具在未设置
`GW_DOMAIN_MODE` 时默认同线程，设置 `GW_DOMAIN_MODE=worker` 可使用 worker。
空值和未知值会被拒绝。

## 接入契约

宿主必须将安装过程与信号注册、ART 信号链修复和应用 JNI 工作串行化。晚接入属于
启动流程。VM 和桥的代码映射保留到进程退出。关闭测试销毁测试用的 HotSpot 后结束进程。

| Host 环境 | 安装的路由 |
| --- | --- |
| 独立 bionic HotSpot | 固定 `libjvm` 的 sigaction、dlsym 导入槽和诊断查询缓存 |
| 同一 Android 进程中的 ART 与 HotSpot | 固定 `libsigchain` 底层 sigaction/sigaction64 回调，保留 VM 导入和 ART 的逻辑用户处理器 |

独立 HotSpot 路径还将诊断查询和缓存接入路由器。诊断继续启用，覆盖曾触发
`handler modified` 告警的查询路径。

ART 路径将域路由器置于已有信号链之前。入口恢复 Host TLS 与 x18，再调用保存的外层
处理器，由它经 ART 转发给 HotSpot。安装时保留 ART 公开的 SIGSEGV 处理器、标志和掩码，
并检查内核入口。校验时调用 `EnsureFrontOfChain` 可能将已转发到 ART 的外层处理器再次插到
ART 下方，从而形成循环。Pojav 复现过重复的 `SignalChain::Handler` 栈帧。外层路由还
覆盖 ART 在调用用户处理器之前读取 pthread TLS 的过程。接入期间须暂停并发信号链修复。

逻辑查询和返回的旧处置会剔除内核拒绝的标志，包括 ART 使用的 `SA_UNSUPPORTED`
探测。晚接入测试覆盖这一行为。

## 制品与失败边界

- 两条路径在访问 profile 指定的数据前校验已映射 JRE 的 SHA256 和文件身份，逐项核对
  槽位对齐、映射所有权、可读且不可执行的权限及原指针值。
- 安装通过原子比较交换更新数据，并恢复页面权限。回滚遇到意外的第三方更新时保留新值。
  无法回滚的夹具进程会退出。
- 可选 Vivo Android 13 ART profile 适用于 JSON 指定散列的 `libsigchain`。
  其他系统或厂商制品需要单独审阅偏移与原值。
- 当前晚接入 HotSpot profile 需要已审阅的诊断缓存符号。早期桌面测试用的 stripped JDK
  缺少该符号，构建器会拒绝该晚接入配置。桌面夹具覆盖独立的路由契约。
- Guest 注册 Host 保留信号时返回 `EBUSY`。在线回收要求实际接收线程提供静止证明。
  参见 [Host 注册回收](../../../core/docs/zh/DOMAINS.md#reclaiming-host-registrations)。

审阅导入路径时使用 [Android 13 sigchain](https://android.googlesource.com/platform/art/+/refs/tags/android-13.0.0_r1/sigchainlib/sigchain.cc) 和
[OpenJDK 17 signals_posix.cpp](https://github.com/openjdk/jdk17u/blob/master/src/hotspot/os/posix/signals_posix.cpp)。
实际偏移绑定到受测二进制的散列。

## 构建与验证

准备匹配的收养 runtime、NDK 和 JRE 制品。可选 ART profile 对应记录中的设备制品。

```sh
python3 android-jni/tests/same-thread/build.py \
  --abi arm64-v8a --system-load --late-signal \
  --art-signal-profile jvm-jni/tests/same-thread/profiles/vivo-android13-sigchain.json \
  --host-libc-signal-profile jvm-jni/tests/same-thread/profiles/vivo-android13-bionic.json \
  --ndk "$ANDROID_NDK" --jdk "$JDK17" --runtime "$DOMAIN_RUNTIME_ARM64"

python3 jvm-jni/tests/same-thread/system-load.py \
  --build jvm-jni/build/tests/late-system-load/arm64-v8a \
  --jdk "$JDK17" --serial "$ANDROID_SERIAL"

python3 jvm-jni/tests/same-thread/android.py \
  --system-load --inline-vm \
  --native jvm-jni/build/tests/late-system-load/arm64-v8a \
  --runtime "$DOMAIN_RUNTIME_ARM64" \
  --jre-assets jvm-jni/build/tests/android/assets/android-jre \
  --jre-libs jvm-jni/build/tests/android/jniLibs/arm64-v8a \
  --gradle "$GRADLE" --sdk "$ANDROID_HOME" --serial "$ANDROID_SERIAL"

python3 jvm-jni/tests/same-thread/late-signal-contract.py
python3 jvm-jni/tests/same-thread/late-signal-contract.py \
  --ndk "$ANDROID_NDK" --serial "$ANDROID_SERIAL"
```

`--inline-vm` 让 ART、HotSpot、bionic 和 GNU Guest 位于同一测试应用进程。省略时，
APK 在应用 UID 的 exec 子进程中运行 native 夹具。两种方式均使用独立的测试应用。

应用内路径要求 `ART_SIGNAL_RECLAIM` 结果。ART 的两个公开注册入口经过已安装的
profile。主 ART 线程执行被替换的普通掩码处理器期间，回收器释放旧映射。随后该线程
从旧回调返回，并成功收到新一代处理器的投递。

shell 测试覆盖两种后端，并在独立 HotSpot 的每个绑定写入之后注入失败，检查回滚和
VM 销毁。成功用例各运行三轮 ClassLoader 装卸。每轮使用八个 native 调用线程执行
8,000 次调用，同时保持一个 Guest 帧，执行 20 轮栈遍历与 GC，并启用 JFR 采样。
计数区分所有 suspend/resume 信号和实际中断 Guest TLS 的信号。每轮同线程另在收养
载体上主动发送 SR/PIPE/XFSZ，检查返回后的 Guest TLS 和 errno。压力结果分别记录
主动信号和 JFR 活动。

日志、清单、散列及受测配置见
[晚接入验证](../../tests/results/same-thread/late-signals/summary.json)。Pojav 和模组
验证由外部验证工作区维护。

[同进程 JNI 性能](PERFORMANCE.md) 使用同一个 APK 测量 worker、同线程和普通 bionic JNI。
性能计时阶段关闭兼容性压力。
