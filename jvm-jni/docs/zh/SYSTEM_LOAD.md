# 同线程域的 System.load 接入

[English](../en/SYSTEM_LOAD.md) | [简体中文](../zh/SYSTEM_LOAD.md)

测试使用生产 JVM adapter 处理真实 `System.load` 和 `System.loadLibrary`，验证 Guest 模块
接入和卸载。夹具默认同线程，设置 `GW_DOMAIN_MODE=worker` 可选择 worker。runner 验证缺省、
worker 和 domain 三种选择，并检查缺省用例实际创建收养域。接入方显式配置 JRE profile，
通过 [运行时启动接口](../../../android-jni/docs/zh/RUNTIME.md) 选择生产后端。

## 实现

`android-jni/tests/same-thread/build.py --system-load` 复用同一传输实现，额外链接本项目生产 `adapter.c`、`profiles.c`、`families.c` 和 `calls.c`。产物独立放在 **jvm-jni/build/tests/system-load/<ABI>/**；Java 辅助类和装载测试归 jvm-jni。

在 `full_host_start` 前注册适配器，配置 `GW_HOST_VM_ADAPTER` 及实际 resident libjava 路径。生产代码校验 libjava/libjvm 散列、所选 VM 与完整私有方法签名，然后安装三种 load/find/unload 钩子。测试模块只声明普通 `Java_*` 导出；调用方经过 VM 的真实符号查找，包含短名称和两个重载长名称。

测试类独立打包，三轮 URLClassLoader 生命周期中重复装载、调用、释放强引用并触发 GC。中间一轮使用 `System.loadLibrary`，其 ClassLoader 的 `findLibrary` 返回测试文件绝对路径；这不代表测试了所有默认 library.path 搜索规则。另一 ClassLoader 同时装载同一映像必须失败。JNI_OnLoad 返回 JNI_ERR 的模块必须回滚，缺少的 JNI 导出必须抛出 UnsatisfiedLinkError。Android 另加载 bionic 控制库，要求走原 VM 装载路径且 Guest 计数不增加。

## 2026-11-15 验证结果

| 环境 | worker | domain | 断言 |
| --- | --- | --- | --- |
| Linux x86_64 / OpenJDK 17.0.21.0.3 | PASS | PASS | 每组 23 项，3 次自动卸载 |
| V2239A / Android 13 / 固定 Android JRE17 / shell | PASS | PASS | 每组 25 项，包含 bionic 透传 |
| 同一手机 / 应用 UID 10310 / linker64 子进程 | — | PASS | 25 项，3 次自动卸载、VM 销毁 |

所有组启用 CheckJNI，最终 active_modules/live_closures 均为零，映像打开/关闭平衡。桌面和 Android 的实测私有接口族分别为 **5** 和 **4**；同为 JRE17 不能据此选同一族。桌面族由 `javap` 和现有固定制品测试复核后显式传入，Android 使用已有制品 profile。

新路径暴露了两点生命周期差异：

1. 存在长期存活的 VM 清理线程时，先停止 Guest worker 会令其进入 DEAD，随后 Host 的空闲检查正确拒绝关闭。现已按既有生产合同改为 **先停止 Host、恢复 VM 钩子，再 join Guest worker**。
2. 同线程模式创建两个域，其中调用线程的域正常析构，VM 清理线程的一个空闲域保留到进程退出。不能在其他线程上执行其 TLS 析构。该行为沿用公开生命周期合同的“空闲 Host binding 可保留”，不宣称所有域资源或整个 GNU runtime 已回收；模块与 JNI 跳板已经卸载。

## 晚加载 DSO 对照

显式晚接入及 ART 同进程夹具使用制品绑定的信号路由，见 [晚接入信号链](LATE_SIGNALS.md)。
以下对照记录了直接装载 DSO 时的处理器和导入状态。

这个对照的 Pojav 启动流程在已有 JVM 中执行 `System.load(libglibc_entry.so)`，
随后调用 `NativeEntry.start`。对应的信号夹具通过主可执行文件导出的 sigaction
在 VM 启动前介入。

`late-signal-probe.py` 在 Android bionic 进程中先安装 Host 处理器，再以 `RTLD_GLOBAL`
装载 signal-router DSO。归档记录了以下观测。

```json
{"result":"UNSUPPORTED","init_status":0,"existing_handler_wrapped":false,"global_sigaction_interposed":false}
```

已有处理器仍未经过域路由。符号字段比较两次动态查找，实际 VM 导入绑定需要独立检查
和接入。

该对照的验证范围是注册与符号绑定。Pojav 及真实模组结果由外部验证工作区维护。

## 复现

从仓库根目录运行。显式指定已构建的收养 glibc runtime 和 SDK、NDK、JDK。

```sh
python3 android-jni/tests/same-thread/build.py --system-load \
  --abi x86_64 --jvm-family 5 --jdk "$JDK17" --ndk "$ANDROID_NDK" \
  --runtime "$DOMAIN_RUNTIME_X86_64"
python3 jvm-jni/tests/same-thread/system-load.py \
  --build jvm-jni/build/tests/system-load/x86_64 --jdk "$JDK17"

python3 android-jni/tests/same-thread/build.py --system-load \
  --abi arm64-v8a --jdk "$JDK17" --ndk "$ANDROID_NDK" \
  --runtime "$DOMAIN_RUNTIME_ARM64"
python3 jvm-jni/tests/same-thread/system-load.py \
  --build jvm-jni/build/tests/system-load/arm64-v8a --jdk "$JDK17" \
  --serial "$ANDROID_SERIAL"
python3 jvm-jni/tests/same-thread/android.py --system-load \
  --native jvm-jni/build/tests/system-load/arm64-v8a \
  --runtime "$DOMAIN_RUNTIME_ARM64" \
  --jre-assets jvm-jni/build/tests/android/assets/android-jre \
  --jre-libs jvm-jni/build/tests/android/jniLibs/arm64-v8a \
  --gradle "$GRADLE" --sdk "$ANDROID_HOME" --serial "$ANDROID_SERIAL"
python3 jvm-jni/tests/same-thread/late-signal-probe.py \
  --ndk "$ANDROID_NDK" --serial "$ANDROID_SERIAL"
```

族 5 仅对应本次已核对的桌面制品；不要对其他 JRE 照搬。Hash 验证失败或签名不匹配必须停止，不通过降低校验强行启用。

[结构化结果](../../tests/results/same-thread/system-load/summary.json)包含两平台结果、应用 APK 散列和晚加载反例；同目录保存两个构建 manifest 及 `logs.json.gz` 完整日志。结果不包含性能测量、其他 JRE、ART JNI 或真实模组兼容性。
