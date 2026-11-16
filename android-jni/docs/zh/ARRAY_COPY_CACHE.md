# 基本类型数组副本缓存

[English](../en/ARRAY_COPY_CACHE.md) | [简体中文](../zh/ARRAY_COPY_CACHE.md)

`src/host/array_copy.c` 为 JNI Get/ReleasePrimitiveTypeArrayElements 提供有上限的 Host 副本缓存。它不返回 JVM 堆裸指针，也不改变两侧 TLS 和 RPC 协议。

## 选择策略

按数组总字节数选择 1 MiB 至 8 MiB 的数组，包含两个边界。最多 8 槽、32 MiB 数据容量，按需分配；预算不包含 malloc 元数据、JVM 堆和回退给 JVM 的临时副本。小数组、超大数组、槽或预算不足以及缓存分配失败均使用真实 JVM 的原始 Elements 路径。

默认自动选择。宿主设置 `GW_HOST_DISABLE_ARRAY_COPY_CACHE` 关闭；该位优先于保留的旧 `GW_HOST_ARRAY_COPY_CACHE` 启用位。不需要修改公开配置结构或 Guest 协议。

缓存持有的是仍处于分配状态的原生缓冲区。Android 分配器可能在 `free` 后保留虚拟地址却归还物理页；再次写入临时副本会触发轻微缺页。复用活跃分配避开这条释放／重新触页路径，不需要更改进程级 allocator 策略或锁定物理页。首次使用仍需分配和触页，实际数组内容仍按 JNI 语义复制。

1 MiB 等于 1,048,576 字节。阈值按总字节数计算，不按元素数量；例如 262,144 个 `int` 元素也达到下限。槽位及总容量上限保持独立，较小数组占满槽时仍可能让其他调用回退。

## 所有权与释放

Host 用自己的 allocator 创建缓冲区，通过真实 JNI GetArrayRegion 填充，返回 `isCopy=JNI_TRUE`。缓冲区只能交回相应 Release 接口，Guest 不能用 free 释放。

| Release mode | 行为 |
| --- | --- |
| 0 | 回写，再归还槽 |
| `JNI_ABORT` | 不回写，归还槽 |
| `JNI_COMMIT` | 回写，继续保有同一个副本，随后仍须最终释放 |

覆盖八种基本类型。释放时通过活跃指针和类型识别所有权，允许有效的跨调用、跨线程释放。缓存不保留 JNIEnv、类加载器或 JNI 对象引用；调用方仍须保证传入数组引用有效。持缓存锁时不调用 JVM。

存在 pending exception 时，回写阶段保存并恢复原异常对象。shutdown 在 Host helper 静止后释放空闲容量；未归还的活跃副本保留并输出诊断，不能强行释放后留下悬空指针。正常退出要求 `active=0 retained_bytes=0`。

## 宿主使用与验证

宿主通过 `gw_host_bridge_config.flags` 选择关闭位。测试应用的 `--array-copy-cache auto|on|off` 只是测试配置入口；产品运行时不读取这些测试标记。

严格语义夹具位于本项目 `tests/native/array_copy_checks.c` 和 `tests/java/benchmark/PayloadModule.java`，由 jvm-jni 的真实 VM 宿主组合运行。覆盖八类型、COMMIT 持有期、释放模式、异常身份、GC、并发租借、跨线程释放和容量回退。运行方法见 [集成测试手册](../../../jvm-jni/tests/README.md)。

缓存减少重复分配和触页，不消除实际内容复制。阈值是当前实现策略，不保证所有 JVM 或负载同样受益。收益验证必须固定 JRE、APK、缓存状态和工作负载，分别检查延迟、缺页、保留容量及完整保存退出；微基准百分比不能直接换算为模组或游戏收益。

本地容量及所有权回归可运行 `python3 android-jni/tests/array_cache.py`（在工作区根目录），使用 ASan/UBSan 验证下限、COMMIT 活跃期、异常路径、12 个借用导致的槽位回退、跨线程归还、32 MiB 预算和清理。`tests/native/allocator_pages.c` 是独立 Android 进程的退页机制诊断：临时切换本进程的 `M_DECAY_TIME`，比较反复分配与保留缓冲区，并用 `mincore` 观察物理页驻留；它不应链接进产品或修改应用的 allocator 策略。

## 机制源码

Android 13 Scudo 的 [`MapAllocatorCache::store`](https://android.googlesource.com/platform/external/scudo/+/8e2d762272cd7bed1362cbe22016d1d9754d74f8/standalone/secondary.h) 在立即回收模式下可以归还物理页并保留虚拟映射；不符合缓存条件的分配则解除映射。[`M_DECAY_TIME` 的实现](https://android.googlesource.com/platform/external/scudo/+/8e2d762272cd7bed1362cbe22016d1d9754d74f8/standalone/wrappers_c.inc) 选择允许范围内的释放间隔。这些源码说明可能发生的机制，不能据此假定所有 Android 厂商、进程或数组大小都采用相同策略。

[OpenJDK 17 的 JNI Elements 实现](https://github.com/openjdk/jdk17u/blob/jdk-17.0.14-ga/src/hotspot/share/prims/jni.cpp#L2235) 为基本类型数组分配并填充副本，最终释放时归还分配。缓存通过保留活跃分配减少重复触页；不能把虚拟地址复用视为物理页仍然驻留，也不能把微基准中的全部数组耗时算作桥接调度开销。
