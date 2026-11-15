# System.load integration with same-thread domains

本轮将已有生产 JVM adapter 直接接入同线程实验后端，通过真实 `System.load` / `System.loadLibrary` 进入 Guest；不再由夹具显式调用公开 loader 的 open/close。当前接入构建默认同线程，`GW_DOMAIN_MODE=worker` 可显式选择 worker；常规 SDK 的默认值和构建入口暂不变，未增加 JRE 自动识别。runner 分别验证未设置选择、显式 worker 和显式 domain；缺省用例检查实际创建了收养域，而不是只看配置字符串。

## Implementation

`android-jni/experiments/same-thread/build.py --system-load` 复用同一传输实现，额外链接本项目生产 `adapter.c`、`profiles.c`、`families.c` 和 `calls.c`。产物独立放在 **jvm-jni/build/experiments/system-load/<ABI>/**；Java 辅助类和装载测试归 jvm-jni。

在 `full_host_start` 前注册适配器，配置 `GW_HOST_VM_ADAPTER` 及实际 resident libjava 路径。生产代码校验 libjava/libjvm 散列、所选 VM 与完整私有方法签名，然后安装三种 load/find/unload 钩子。测试模块只声明普通 `Java_*` 导出；调用方经过 VM 的真实符号查找，包含短名称和两个重载长名称。

测试类独立打包，三轮 URLClassLoader 生命周期中重复装载、调用、释放强引用并触发 GC。中间一轮使用 `System.loadLibrary`，其 ClassLoader 的 `findLibrary` 返回测试文件绝对路径；这不代表测试了所有默认 library.path 搜索规则。另一 ClassLoader 同时装载同一映像必须失败。JNI_OnLoad 返回 JNI_ERR 的模块必须回滚，缺少的 JNI 导出必须抛出 UnsatisfiedLinkError。Android 另加载 bionic 控制库，要求走原 VM 装载路径且 Guest 计数不增加。

## Recorded result: 2026-11-15

| 环境 | worker | domain | 断言 |
| --- | --- | --- | --- |
| Linux x86_64 / OpenJDK 17.0.21.0.3 | PASS | PASS | 每组 23 项，3 次自动卸载 |
| V2239A / Android 13 / 固定 Android JRE17 / shell | PASS | PASS | 每组 25 项，包含 bionic 透传 |
| 同一手机 / 应用 UID 10310 / linker64 子进程 | — | PASS | 25 项，3 次自动卸载、VM 销毁 |

所有组启用 CheckJNI，最终 active_modules/live_closures 均为零，映像打开/关闭平衡。桌面和 Android 的实测私有接口族分别为 **5** 和 **4**；同为 JRE17 不能据此选同一族。桌面族由 `javap` 和现有固定制品测试复核后显式传入，Android 使用已有制品 profile。

新路径暴露了两点生命周期差异：

1. 存在长期存活的 VM 清理线程时，先停止 Guest worker 会令其进入 DEAD，随后 Host 的空闲检查正确拒绝关闭。现已按既有生产合同改为 **先停止 Host、恢复 VM 钩子，再 join Guest worker**。
2. 同线程模式创建两个域，其中调用线程的域正常析构，VM 清理线程的一个空闲域保留到进程退出。不能在其他线程上执行其 TLS 析构。该行为沿用公开生命周期合同的“空闲 Host binding 可保留”，不宣称所有域资源或整个 GNU runtime 已回收；模块与 JNI 跳板已经卸载。

## Original late-DSO counterexample

后续已增加显式晚接入及 ART 同进程夹具，见 [LATE_SIGNALS.md](LATE_SIGNALS.md)。以下保留原始反例：**仅 dlopen 一个 DSO** 仍不提供符号接管能力。新实现依赖显式、制品绑定的信号接入，不能把这些条件省略。

外部 Pojav 接入位于 `glibc-worker-validation/pojav`。其 `GuestAgent.premain` 在已有 JVM 中 `System.load(libglibc_entry.so)`，随后调用 NativeEntry.start。当前信号路由则依靠**主可执行文件导出的 sigaction 在 VM 启动前介入**。

`late-signal-probe.py` 在真实 Android bionic 进程中先安装 Host 处理器，再以 RTLD_GLOBAL 装载同一 signal-router 源码生成的 DSO。它只检查注册状态，不投递危险信号、不运行游戏。结果为：

```json
{"result":"UNSUPPORTED","init_status":0,"existing_handler_wrapped":false,"global_sigaction_interposed":false}
```

因此仅替换 Pojav premain 的桥 DSO 不成立：已有处理器没有获得 TLS 恢复入口，已有 libc 符号绑定也未被后来装载的 DSO 替代。`signal_router_init == 0` 只表示当前初始化函数成功，不能作为晚接入能力判断。

**该轮没有替换 Pojav 安装包或启动真实模组。** 后续的显式信号接入需要：确保实际 VM 的注册与查询进入路由，同时保留原处理器、原掩码及 ART/Host 的信号链。简单提前 dlopen 一个 DSO 也未必能改变 bionic 的符号绑定，不能将其当作已经验证的方案。此问题独立于 JNI 装载钩子的通过结果；尚不支持宣称 Pojav 已可直接采用。

## Reproduction

从仓库根目录运行；runtime 为前一轮已构建的收养补丁 glibc，SDK/NDK/JDK 路径显式填写：

```sh
python3 android-jni/experiments/same-thread/build.py --system-load \
  --abi x86_64 --jvm-family 5 --jdk "$JDK17" --ndk "$ANDROID_NDK" \
  --runtime "$DOMAIN_RUNTIME_X86_64"
python3 jvm-jni/experiments/same-thread/system-load.py \
  --build jvm-jni/build/experiments/system-load/x86_64 --jdk "$JDK17"

python3 android-jni/experiments/same-thread/build.py --system-load \
  --abi arm64-v8a --jdk "$JDK17" --ndk "$ANDROID_NDK" \
  --runtime "$DOMAIN_RUNTIME_ARM64"
python3 jvm-jni/experiments/same-thread/system-load.py \
  --build jvm-jni/build/experiments/system-load/arm64-v8a --jdk "$JDK17" \
  --serial "$ANDROID_SERIAL"
python3 jvm-jni/experiments/same-thread/android.py --system-load \
  --native jvm-jni/build/experiments/system-load/arm64-v8a \
  --runtime "$DOMAIN_RUNTIME_ARM64" \
  --jre-assets jvm-jni/build/tests/android/assets/android-jre \
  --jre-libs jvm-jni/build/tests/android/jniLibs/arm64-v8a \
  --gradle "$GRADLE" --sdk "$ANDROID_HOME" --serial "$ANDROID_SERIAL"
python3 jvm-jni/experiments/same-thread/late-signal-probe.py \
  --ndk "$ANDROID_NDK" --serial "$ANDROID_SERIAL"
```

族 5 仅对应本次已核对的桌面制品；不要对其他 JRE 照搬。Hash 验证失败或签名不匹配必须停止，不通过降低校验强行启用。

[结构化结果](results/system-load/summary.json)包含两平台结果、应用 APK 散列和晚加载反例；同目录保存两个构建 manifest 及 `logs.json.gz` 完整日志。结果不包含性能测量、其他 JRE、ART JNI 或真实模组兼容性。
