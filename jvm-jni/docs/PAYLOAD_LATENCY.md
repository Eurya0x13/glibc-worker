# JNI 数据量与延迟测试方法

## 对照范围

同一个 Android/JRE 测试应用内，A 为普通 bionic JNI，B 为 GNU worker；同源 C 夹具分别编译。覆盖指针加长度、DirectBuffer 地址查询、memcpy、GetByteArrayRegion 和 Get/ReleaseByteArrayElements。指针控制项不读取声明长度的全部数据，复制项才触达内容。

B−A 包含不同 libc、编译器、allocator、内存位置、VM 工作和跨线程调度，不能全部称为桥接成本。Elements 的复制/分配不是纯 memcpy 带宽。采用 [数组缓存](../../android-jni/docs/ARRAY_COPY_CACHE.md) 时须保持配置一致。

## 执行

先构建所选 ABI 的普通配置 APK，再执行同 APK 十对 A/B：

```sh
GW_ABI=arm64-v8a GW_RPC_PROFILE=0 GW_RPC_SPIN_NS=20000 \
  GW_FUTEX_WAKE_FILTER=0 GW_RPC_READY_POLL=0 \
  jvm-jni/tests/android/gradlew -p jvm-jni/tests/android :app:assembleDebug
python3 jvm-jni/tests/coverage/compare_payload.py --device SERIAL \
  --apk jvm-jni/build/tests/android/app/outputs/apk/debug/app-debug.apk \
  --output jvm-jni/build/test-results/payload-comparison
```

`compare_array_cache.py` 在同一 Guest 路径内比较缓存关闭/自动选择；`--payload-profile threshold` 增加阈值两侧的大小。正式计时使用普通 JIT、CheckJNI 关闭；严格语义检查单独运行，不能混合两种模式的读数。

## 样本和校验

默认每侧十轮新进程，顺序平衡；每对使用同一配置乱序种子。40 个配置各三次 trial，每次 300 预热、500 计时。分配、预触页、填充与完整数据核对在计时外；接口自身的分配和复制仍在计时范围。

每轮保存原始样本、分位数、≥1ms/≥5ms 计数、模式、APK 散列与调度状态。`tests/payload.py` 验证样本数量及摘要一致性；不完整轮次不能作为完整十对。分析逐对差异，说明汇总顺序；trial 分位数的中位数不是合并样本后的 p99。

## 诊断与解释

单次运行可加 `--payload-diagnostic` 记录 native 阶段时钟和 Host getrusage。`analyze_payload.py` 分析样本；`capture_payload.py` 采集同期调度轨迹。诊断会增加成本，与关闭诊断的正式 A/B 分开。

以数据量增长区分固定控制成本与实际复制成本，以 minor fault、线程 CPU、trace 区分分配/触页和调度。不能只看低 CPU 占比排除长尾，也不能把墙钟减 CPU 全称为调度等待。

当前夹具只有一个 Java 调用者，重复使用预触页缓冲区，引用在配置时建立；不覆盖冷缓存、每调用新建数组、所有并发模式或真实游戏帧率。采用优化前仍须在实际模组验证触发频率、端到端延迟、保留容量与保存退出。
