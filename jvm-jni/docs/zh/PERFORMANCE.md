# 同进程 JNI 性能

[English](../en/PERFORMANCE.md) | [简体中文](../zh/PERFORMANCE.md)

此测试通过真实 JVM `System.load` 适配器装载模块，测量同步 native 调用。ART、HotSpot、
bionic 和 GNU Guest 位于同一 Android 应用进程，采用已审阅的固定信号 profile。
结果适用于受测制品和工作负载。参见 [晚接入信号链](LATE_SIGNALS.md)。

## 复现

按晚接入指南构建并安装应用内夹具。默认安装后的用例检查 CheckJNI、信号和生命周期。
完成检查后运行性能测试。

```sh
python3 jvm-jni/tests/same-thread/performance.py \
  --serial "$ANDROID_SERIAL" \
  --build jvm-jni/build/tests/late-system-load/arm64-v8a \
  --pairs 10 --threads 1 8
```

runner 校验 native 输入和已安装 APK 的散列。每轮启动新应用进程，交替 worker 和同线程顺序，
并为每种并发度保留三轮普通 bionic JNI 参考。沿用现有亲和性、调度策略、自旋配置和生产默认值。
测试仅重启夹具应用，保存每轮前后的温度和 Android 热状态。测量期间保持设备空闲。

## 工作负载与计时

两种后端使用相同的 Guest 函数，覆盖标量返回、一次 Java 回调、四层 Java/native 回调，
用 `GetByteArrayElements` 读取并累加 128 字节和 64 KiB 数组，以及 10,000 步原生整数递推。
普通 JNI 参考用 Android 编译器构建同一夹具源码，使用宿主原生装载。编译器和 libc 的差异
也会进入该参考值，比较时应保留这一条件。

每项使用 1 或 8 个 Java 调用线程，各预热 8 批，再测 30 批。每批 256 次调用，
大数组每批 64 次。所有线程完成预热后开始收集进程 CPU 与切换次数，全部完成批次后结束。
窗口内包含 Guest worker、VM/ART 后台工作、同步和调度。八线程时，墙钟除以完成调用数
给出吞吐量的倒数。初始化和 ClassLoader 清理发生在窗口之外。

另有独立阶段为每个线程的 512 次调用单独计时。P99 包含时钟调用及移动端调度波动。
各线程可以在不同时间完成两个阶段，因此阶段结束前并发度会下降。

runner 校验返回值、校验和、实际装载路径、ClassLoader 卸载、零活动模块和跳板、
VM 销毁及 ART 信号链。计时期间关闭 CheckJNI、JFR、主动信号探针和强制 GC。
强制 GC 仅在计时后验证卸载。夹具使用同步 JIT、关闭分层编译并配置较低的编译阈值。

## 测量结果

`jvm-jni/build/tests/late-system-load/arm64-v8a/performance` 下的每个运行目录保存
native 制品身份、APK 散列、设备状态、压缩原始输出和结构化结果。统计使用配对比值，
保留原始离群值，并报告改善的配对数。完整十对测量及对应制品见
[性能归档](../../tests/results/same-thread/performance/REPORT.md)。

用完整采集结果生成报告。

```sh
python3 jvm-jni/tests/same-thread/report-performance.py "$CAPTURE" \
  --output jvm-jni/build/tests/late-system-load/arm64-v8a/performance-report
```
