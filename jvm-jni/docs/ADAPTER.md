# HotSpot 私有装载适配器

本项目使 HotSpot 的私有加载、查找和卸载路径调用 android-jni 的版本化接口。它是可选接入层，不属于 core 或公开 JNI 桥。

## 制品与构建输入

两种配置方式都要求明确指定接口族及审阅后的 libjava、libjvm 两个 SHA256：

1. 构建时设置 `GW_JRE_ADAPTER_HEADER`，头文件定义 `GW_JVM_PROFILE_NAME`、`GW_JVM_LIBJAVA_SHA256`、`GW_JVM_LIBJVM_SHA256`、`GW_JVM_PROFILE_FAMILY`。
2. 在安装前调用 `gw_jvm_jni_set_profile`；它复制 v2 结构，不持有调用方指针，已安装时返回 BUSY。

```c
struct gw_jvm_profile_v2 profile = {
    .abi_version = GW_JVM_PROFILE_ABI_V2,
    .struct_size = sizeof profile,
    .family = GW_JVM_NATIVE_LIBRARIES_19,
    .name = "reviewed-build-name",
    .libjava_sha256 = "<64 lowercase hex digits for reviewed libjava>",
    .libjvm_sha256 = "<64 lowercase hex digits for reviewed libjvm>"};
/* Check GW_OK before starting the public bridge. */
int result = gw_jvm_jni_set_profile(&profile, &error);
```

上例的散列是占位符，不能直接启用。产品应从自己的已审阅制品清单选取 profile；运行时 API 不提供额外的信任清单，也不会自动把当前文件散列当成兼容证明。只匹配散列仍不足以识别参数语义，必须选择正确接口族。

| 枚举 | 精确边界 |
| --- | --- |
| `GW_JVM_CLASSLOADER_8` | 8–9 形态，实例 load/find/unload，成功时设置 loaded |
| `GW_JVM_CLASSLOADER_10` | 10–14 GA 形态，load0 两参数 |
| `GW_JVM_CLASSLOADER_10_THROW` | 11u 回移植形态，load0 增加 throwExceptionIfFail |
| `GW_JVM_NATIVE_LIBRARIES_15` | 15–17 GA 形态，load 含 isJNI |
| `GW_JVM_NATIVE_LIBRARIES_18` | 18 GA / 17u 回移植形态，load 含 isJNI 与 throwExceptionIfFail |
| `GW_JVM_NATIVE_LIBRARIES_19` | 19+ 形态，查找入口在 NativeLibrary，load 不再含 isJNI |

这些是接口契约名，不是按主版本自动选择的规则。源码边界依据见 [兼容性契约](JRE_COMPATIBILITY.md)。JDK28 不进入运行支持矩阵。

测试默认 profile 由 `tests/scripts/prepare-jre.py` 按 [固定 Android 制品](../tests/config/android-jres.json) 生成。测试宿主的 ABI、归档、libjvm 和 libjava 必须整体匹配。只支持通用的 `GW_JVM_*` 构建宏、`jvm_adapter.h` 与 `JvmAdapter`；旧 JRE17 名称的 C/Java 转发入口及 `GW_HOST_LEGACY_JRE17` 已删除，不提供源码或二进制别名兼容。

独立构建方法见 [项目入口](../README.md)。Java 辅助类编译时依赖已安装的 android-jni helpers JAR，本项目不复制其公共类。

v1 profile 的 ABI/大小明确拒绝，不自动补齐新增散列。调用者必须同步更新公开结构和两个构建散列输入。

制品文件在校验、安装和使用期间必须保持不可变；散列门槛不提供针对文件替换竞态或恶意 native 的安全隔离。

## 生命周期

加载 `libgw_jvm_jni.so` 的构造函数只注册适配器；需要时也可显式调用 `gw_jvm_jni_register`。注册必须在公共 bridge 初始化前完成。宿主通过配置请求 `GW_HOST_VM_ADAPTER` 后才执行 install，shutdown 时执行 restore。

install 分为辅助类解析、libjava 散列校验、已驻留映像/目标 VM 归属及其 libjvm 散列确认、字段解析、三个原方法完整解析及注册。只接受已经驻留的 libjava，不为探测而加载另一套 libjava/JVM；枚举入口和目标 JavaVM 的 GetEnv 必须属于同一 VM 映像，枚举出的实例必须与当前 JNIEnv 的目标相同。三个原方法导出必须来自所校验的 libjava 文件，不能从依赖库借同名符号。全部验证完成前不修改 VM。RegisterNatives 失败时尝试恢复全部原函数；若回滚仍失败，保留句柄和全局引用，阻止重配 profile，宿主必须丢弃进程。真实 JRE8/11/17/21/25 的独立测试进程已覆盖注册部分失败和恢复再次失败。

设置 profile、install 和 restore 均须在没有并发装卸载及调用的状态下进行。互斥锁保护配置事务，不替调用方暂停 VM 的其他线程。restore 前关闭所有 Guest 模块并停止调用，恢复后才可释放桥；公共 Host shutdown 已遵循这个顺序。

GNU 分支整体委托给后端处理 JNI_OnLoad/OnUnload；原 VM 路径不得再重复调用生命周期函数，或把 Guest 逻辑句柄交给 JVM_UnloadLibrary。普通 bionic 库继续走原加载规则。ClassLoader 上下文、路径检查与所有权仍由目标 VM 管理。

带 throwExceptionIfFail 的接口只把该标志用于装载失败策略；JNI_OnLoad 拒绝、版本错误和已有 Java 异常仍保留失败语义。builtin 及旧接口中的非 JNI 装载继续调用原函数。

## 方法签名与后续源码接入

JNI 跳板必须知道完整方法描述符。当前私有适配器包含反射签名解析，多个重载共享短 JNI 导出等情况仍有限制。源码集成应在 VM 持有真实 method metadata 的位置把描述符显式传入后端 `find`，不能只替换 dlopen/dlsym 并猜函数原型。

19+ 查找入口只传 handle，不再传库对象。辅助类在装载提交时将 handle 关联到调用方 Class 的弱引用，按其 ClassLoader 解析签名；卸载和失败回滚时删除关联。不能使用强引用，否则会阻止 ClassLoader 回收和自动卸载。测试通过两个完整的回收、卸载、再次加载周期验证此约束。

OpenJDK 8–25 的装载接口可分为若干版本族，且维护分支存在参数回移植，见 [JRE 兼容性契约](JRE_COMPATIBILITY.md)。不能把固定旧制品的钩子直接套到其他 JRE。该项目没有宣称已实现通用 JRE 源码补丁或 ART 私有接口。

## 运行时组合

模块化使用四个独立安装包；旧宿主可选择 `scripts/package-runtime.sh` 组装的单 DSO `libglibc_bridge.so` 和合并 helper JAR。同一进程只能选择一种组合，避免两套单例状态。兼容制品包含公共桥和适配器，但不包含测试 Activity、JNI 夹具或测试 JRE 路径缓存补丁。

Android/JRE 测试宿主按制品选择 Pojav JLI 启动夹具或已固定的 JRE17 路径缓存修补。目录部署、linker64 子进程探针和堆指针标记兼容库都只属于测试应用，不进入 SDK；已有 JVM 宿主只接入适配器，不重复创建 VM。启动细节与独立对照见 [测试手册](../tests/README.md)。
