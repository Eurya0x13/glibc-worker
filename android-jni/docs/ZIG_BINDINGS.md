# Host 资源存储

## 范围与构建

Host 的 binding、引用/ID 槽位和注册类记录固定使用 Zig 存储。构建需要精确版本 Zig 0.16.0；C 定容后端在完成桌面与 ARM64 真机对照后已删除。`GW_BINDING_BACKEND` 默认 `zig`，旧值 `c` 明确报错。公开 C ABI、JNI 调用、C atomics、锁和 libffi 构造/销毁继续由 C 薄层负责。core、GNU loader、Guest 和 JVM 私有适配器没有因存储变更迁移语言。

[build.zig](../build.zig) 是 Host 存储对象的唯一编译规则，固定 ReleaseSafe、PIC、Host allocator 和无额外 TLS/runtime。CMake 调用该构建图并负责 C/JNI、共享库/静态库和 SDK 安装；安装后的 C/C++ SDK 消费者不需要 Zig。

```sh
# android-jni 项目目录内
zig build host-store --prefix build/host-store -Dcpu=baseline
zig build host-store --prefix build/host-store-arm64 \
  -Dtarget=aarch64-linux-android.29 -Dcpu=baseline

# 工作区根目录内
GW_ABI=arm64-v8a scripts/build-runtime.sh --module jvm-jni
```

Python 负责测试编排、报告和依赖准备；四个项目尚未整体迁移到 Zig 构建图。缓存与安装前缀均归各项目的 `build/`。

## 容量与增长

| 资源 | 初始容量 | 耗尽后 |
| --- | --- | --- |
| 顶层 Guest 模块 | 固定 64 槽 | 返回 `GW_E_CAPACITY`，不扩容 |
| JNI binding 存储 | 全局共享 512 槽 | 按 64 槽增加稳定块 |
| 引用与 methodID/fieldID | 共用 16,384 槽 | 按 64 槽增加稳定块 |
| 注册类所有权记录 | 共用 256 槽 | 按 64 槽增加稳定块 |

默认值集中在 [resource_limits.h](../src/host/resource_limits.h)，模块上限在 [module_id.h](../../bionic-dlopen/include/module_id.h)。资源 store 首次使用时预留初始存储；这不创建对应数量的 libffi closure、VM 引用或注册类，也不预先创建所有模块索引。以后仅在没有可用槽位时增加一块；块指针、哈希表和 owner 向量采用几何增长，避免逐对象堆分配。

binding 的全部模块共用一个存储池；每模块的哈希表索引 Guest 地址和完整描述符，每模块连续槽位向量用于释放。64 个模块各使用 8 个不同入口仍只占 512 槽，不能给每个模块单独预留 512 槽。释放单个 binding 会将空位交还共享池；模块退出也不会留下只能由该模块复用的存储块。

已发布对象、描述符、CIF 和参数类型数组不随增长移动。调用热路径直接携带 binding 指针，不扫描存储或经过哈希查找。引用令牌通过索引直接定位块；local 成员关系使用连续 token 向量和 swap-remove，回填被移动成员的位置，不使用链表。

引用令牌的高 32 位是代数，低 32 位是索引加一，零表示 null。达到 `UINT32_MAX` 后永久退役该槽，不能回绕复活旧 token。引用/注册池可表示最多 4,294,967,232 个槽位，实际首先受可分配内存限制；binding 受地址空间、分配器及哈希表表示范围限制，没有另设业务硬上限。令牌仅在当前 store/桥生命周期内有效，不得跨关闭或进程持久化，也不能解引用。

空块保留到 store 销毁并优先复用；释放不自动收缩峰值容量，避免地址搬迁、代数重置及频繁分配。初始容量不是性能上限：ID 去重、类身份匹配和清扫仍扫描已分配容量，并用 JNI `IsSameObject` 判断身份。动态扩容没有消除这些路径的成本，也不作为性能提升证据。单 Guest、单次注册 64 方法、255 字节描述符、上下文和重入限制继续适用。

## 所有权、失败和释放

所有分配经 Host C allocator 回调，释放回到同一 libc。Zig 不使用默认堆、线程、TLS、锁或进程启动 API；内部符号隐藏。ReleaseSafe 不变量错误使用 trap，不引入额外 panic/栈回溯运行时。

插入先预留块、索引和 owner 向量，再发布对象或令牌。分配失败不发布半成品；已有对象、token 和 owner 成员关系保持有效。初始化预留中途失败会释放已分配的块。失败操作可能保留已增长的空闲容量；它不表示对象已经创建。删除不分配内存；有存活对象时拒绝销毁 store。

存储分配失败后，原始 VM 引用仍由 C 调用方持有。native 参数可能是借用 local，不能由存储层随意删除。生成的 NewGlobalRef/NewWeakGlobalRef 包装会在 token 分配失败后删除刚创建的 VM 引用；ID 插入失败也删除本次 weak class 引用。描述符先检查再完整复制，不以截断键继续。资源耗尽的 Java 异常尚未完全统一，见工作区 AUD-03。

RegisterNatives 在交给 VM 前失败会撤回本批新建 binding 与未发布类预留。并发预留使用 pending 计数；VM 返回 JNI_ERR 时可能已发布部分入口，相关记录和 closure 必须保留到模块清理。弱类引用不主动固定 ClassLoader。

释放仍要求 VM 不会再次进入旧入口，且 Guest 后台工作已停止。在途计数无法保护尚未进入 closure 的 VM 线程。不确定的清理失败会保留可能可达的资源并永久停用桥，要求宿主丢弃进程，见 [装载契约](LOADER_BACKEND.md)。扩容与普通生命周期测试不证明任意并发卸载安全。

## 验证

```sh
# Linux：Zig 0.16.0、Clang、pkg-config/libffi、桌面 JDK
python3 android-jni/tests/bindings.py
python3 android-jni/tests/resources.py

# Android：默认构建即使用 Zig
GW_ABI=arm64-v8a \
  jvm-jni/tests/android/gradlew -p jvm-jni/tests/android :app:assembleDebug
jvm-jni/tests/coverage/run.sh --device SERIAL --reuse-apk --apk APK --output OUTPUT
```

桌面测试允许显式指定安装的 core/dlopen 头文件及 JDK。C/JNI 测试使用 ASan/UBSan，Zig 对象使用 ReleaseSafe，不宣称 Zig 对象经过 ASan 全量插桩。

验证包括共享初始池、跨初始容量增长、增长前后地址稳定、空位复用、初始化及插入 OOM、永久退役和 owner swap-remove。压力范围为 8,192 个存储 binding、4,096 个生产 binding、扩容期间旧入口的 10,000 次并发调用、1,024 个注册类及 32,768 个同时存活引用；最终检查资源平衡。C loader 独立测试实际填满 64 个模块槽，拒绝第 65 个，验证高槽位查找与旧代数拒绝。

Android 固定 JRE17/ARM64 夹具每轮调用 1,024 个不同 JNI 方法，为 320 个类注册 native，并创建/释放 20,000 个 global 引用。三个 ClassLoader 周期分别输出 JSON 容量及描述符结果；任何缺失、值错误或退出后引用/closure 未归零均失败。它不等于 ART、其他 Android JRE 或任意模组已验证，也没有测量性能收益。

本轮 C/Zig 对照和删除 C 后默认构建的结果分别保存在各项目 `build/aud01/`，不再维护可构建的 C 后端。原 C 实现可从 Git 父提交 `8975fc0` 复核。
