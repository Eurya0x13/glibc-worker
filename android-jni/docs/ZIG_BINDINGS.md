# Zig Host 资源管理

## 范围与构建

`GW_BINDING_BACKEND=zig` 选择 Host 端 binding、引用/ID 槽位及注册类记录的 Zig 存储。默认 `c` 保留固定容量对照。公开 C ABI 和共享 POD/原子协议不变；JNI 调用、C atomics、锁、libffi 构造与销毁仍由 C 负责。core、GNU loader、Guest 和 JVM 私有适配器没有因本选项迁移语言。

需要精确版本 Zig 0.16.0。项目根目录的 [build.zig](../build.zig) 是 Zig 对象的唯一编译规则，固定 ReleaseSafe、PIC、Host allocator 和无额外 TLS/runtime 的约束。CMake 调用该构建图并负责 C/JNI 编译、共享库/静态库和 SDK 安装；不再通过 Python 脚本拼接 Zig 编译参数。下游 C/C++ SDK 消费者不需要安装 Zig。

```sh
# 在 android-jni 项目目录内单独生成 Host 对象。
zig build host-store --prefix build/host-store -Dcpu=baseline
zig build host-store --prefix build/host-store-arm64 \
  -Dtarget=aarch64-linux-android.29 -Dcpu=baseline

# 工作区组合构建；独立项目也可向 CMake 传同名 -D 选项。
GW_ABI=arm64-v8a GW_BINDING_BACKEND=zig scripts/build-runtime.sh --module jvm-jni
```

`build.zig` 目前只接管 Zig 资源管理对象；整个四项目工具链尚未替换为 Zig 构建图。Python 保留测试编排、结构化报告和依赖准备职责。构建缓存与安装前缀均在本项目 `build/` 内。

## 存储与索引

| 资源 | C 控制后端 | Zig 后端 |
| --- | --- | --- |
| JNI binding | 全局 512 槽 | 每模块哈希索引与稳定存储块，按需增长 |
| 引用及 methodID/fieldID | 共用 16,384 槽 | 稳定分块槽位池，按需增长 |
| 注册类所有权记录 | 共用 256 条 | 使用独立的可增长槽位池 |
| local 引用成员关系 | 每上下文连续 token 数组 | 每上下文连续 token 数组 |

binding 每块连续放置 64 个 C 对象；位图分配空位，连续数组保存块指针。已发布的 binding、描述符、CIF 和参数类型数组不会因扩容而移动。binding 查找键为模块、Guest 地址和完整描述符；调用热路径直接携带 binding 指针。

通用槽位池也按 64 个对象分块，位图记录存活和永久退役槽位。64 是增长粒度。增加块、扩展空闲块索引或 owner 数组，不移动现有对象；删除不分配内存。local 成员关系采用连续 token 数组和 swap-remove，通过槽位中的位置回填索引，不使用链表，也不为每个引用分配对象。

引用令牌使用高 32 位代数、低 32 位索引加一，零为 null。每次复用提升代数；达到 `UINT32_MAX` 后永久退役槽位，不能回绕复活旧 token。Zig 槽位池的索引表示上限为 4,294,967,232 个槽位，实际首先受可分配内存限制。令牌只在当前 store/桥生命周期内有效，不能跨关闭或进程持久化，也不是可以解引用的指针。Host/Guest 应成套部署。

ID 的类身份仍以 JNI `IsSameObject` 判断，不能把 jobject 地址当作 Java 对象身份。ID 去重、类身份查找和清扫当前仍扫描已分配容量；扩容不等于这些路径已改为哈希查找。共享锁、单 Guest、32 个顶层模块、单次注册 64 个方法、描述符 255 字节、上下文和重入限制继续适用。扩容本身不作为性能提升证据。

源码职责：

- [binding_store.zig](../src/host/binding_store.zig)：binding 的模块/函数索引和稳定存储。
- [slot_store.zig](../src/host/slot_store.zig)、[slot_store.h](../src/host/slot_store.h)：通用槽位、代数和连续 owner 成员索引的私有 ABI。
- [slot_allocator.c](../src/host/slot_allocator.c)、[binding_table_zig.c](../src/host/binding_table_zig.c)：Host allocator 与对象构造适配。
- [references.c](../src/host/references.c)：VM 引用、ID、帧和弱类缓存的生命周期。
- [bindings.c](../src/host/bindings.c)：JNI/libffi 执行、类所有权和注册事务。
- [slot_store.c](../src/host/slot_store.c)、[binding_table.c](../src/host/binding_table.c)：C 对照实现。

## 失败与释放契约

所有分配通过 C 提供的 Host allocator 回调，释放回到同一 libc。Zig 不使用默认堆、线程、TLS、锁或进程启动 API；内部入口隐藏。ReleaseSafe 的内部不变量错误使用 trap，不引入额外 panic/栈回溯运行时。

插入先预留索引、owner 数组和块空间，再发布对象/令牌；失败不产生半成品成员关系。存活对象存在时拒绝销毁 store。空块保留至 store 销毁，以避免重置代数；删除不立即缩减峰值容量。

引用槽位分配失败时原始 VM 引用仍属于 C 调用方：native 参数可能是借用的 local，不能由存储层随意删除。生成的 `NewGlobalRef` / `NewWeakGlobalRef` 包装会在 token 分配失败后删除刚创建的 VM 引用。ID 插入失败也删除本次新建的 weak class 引用。描述符在反射、命令、ID 和 binding 插入前检查长度，不把截断结果作为有效键。

`RegisterNatives` 在交给 VM 前失败，会撤回本批新建 binding 和未发布的类所有权预留。并发预留通过 pending 计数区分；不会撤回其他已发布或仍在准备的注册。VM 返回 `JNI_ERR` 后可能已经发布部分入口，记录与 closure 保留至模块清理。类记录使用 weak 引用，不主动固定 ClassLoader。

释放仍要求 VM 不会再次进入旧入口，且 Guest 后台工作已经停止。在途计数不能保护尚未进入 closure 的 VM 线程。`UnregisterNatives` 或其他不确定清理失败会保留可能仍被引用的资源并永久停用桥，由宿主丢弃进程，见 [装载契约](LOADER_BACKEND.md)。不能用扩容测试证明任意并发卸载安全。

## 验证

```sh
# Linux：Zig 0.16.0、Clang、pkg-config/libffi 和桌面 JDK。
python3 android-jni/tests/bindings.py
python3 android-jni/tests/resources.py

# Android：显式选择设备、固定 APK 和本轮独立输出目录。
GW_ABI=arm64-v8a GW_BINDING_BACKEND=zig \
  jvm-jni/tests/android/gradlew -p jvm-jni/tests/android :app:assembleDebug
jvm-jni/tests/coverage/run.sh --device SERIAL --reuse-apk --apk APK --output OUTPUT
```

测试可用 `--core-include`、`--dlopen-include` 指定已安装的依赖头文件，`--jdk` 指定桌面 JNI 头。C 测试与桥接部分使用 ASan/UBSan；Zig 对象使用 ReleaseSafe，不宣称它经过 ASan 全量插桩。VM/RPC 替身测试没有创建 Android 双 libc 环境。

本机验证包括 8,192 个存储 binding、4,096 个生产 binding、扩容期间旧入口的 10,000 次并发调用、1,024 个注册类、32,768 个同时存活引用、类语义身份去重/回收、帧释放、陈旧令牌和缩小代数上限后的永久退役。binding 和槽位分别注入 40/48 个分配失败位置场景，并检查最终资源平衡；C 对照检查各固定上限。

Android 固定 JRE17 / ARM64 夹具每轮调用 1,024 个不同 JNI 方法、为 320 个不同类实际注册 native，并同时创建/释放 20,000 个 global 引用。三个 ClassLoader 周期分别输出 JSON `BINDING_CAPACITY_TEST` / `RESOURCE_CAPACITY_TEST`，额外通过 `AUDIT_DESCRIPTOR_TEST` 验证 255 字节反射方法描述符接受及超长方法/字段描述符拒绝。任何一轮缺失、返回值错误或退出后引用/closure 未归零都会使覆盖失败。它不等于所有 Android JRE、ART 或所有模组已验证。

当前结论为 GO：可以继续使用可选 Zig Host 资源后端。默认后端仍为 C；本轮未测量性能收益。原始结果与 APK 散列写入各项目 `build/`，不把运行日志混入开发者文档。
