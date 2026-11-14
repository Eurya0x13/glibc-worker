# 基本类型数组副本缓存

`host/array_copy.c` 为 JNI Get/ReleasePrimitiveTypeArrayElements 提供有上限的 Host 副本缓存。它不返回 JVM 堆裸指针，也不改变两侧 TLS 和 RPC 协议。

## 选择策略

按数组总字节数选择 2 MiB 至 8 MiB 的数组，包含两个边界。最多 8 槽、32 MiB 数据容量，按需分配；预算不包含 malloc 元数据、JVM 堆和回退给 JVM 的临时副本。小数组、超大数组、槽或预算不足以及缓存分配失败均使用真实 JVM 的原始 Elements 路径。

默认自动选择。宿主设置 `GW_HOST_DISABLE_ARRAY_COPY_CACHE` 关闭；该位优先于保留的旧 `GW_HOST_ARRAY_COPY_CACHE` 启用位。不需要修改公开配置结构或 Guest 协议。

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

严格语义夹具位于本项目 `tests/native/array_copy_checks.c` 和 `tests/java/benchmark/PayloadModule.java`，由 jvm-jni 的真实 VM 宿主组合运行。覆盖八类型、COMMIT 持有期、释放模式、异常身份、GC、并发租借、跨线程释放和容量回退。运行方法见 [集成测试手册](../../jvm-jni/tests/README.md)。

缓存减少重复分配和触页，不消除实际内容复制。阈值是当前实现策略，不保证所有 JVM 或负载同样受益。收益验证必须固定 JRE、APK、缓存状态和工作负载，分别检查延迟、缺页、保留容量及完整保存退出；微基准百分比不能直接换算为模组或游戏收益。
