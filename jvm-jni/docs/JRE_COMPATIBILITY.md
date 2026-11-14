# JRE 装载适配契约与版本族

## 当前支持边界

Android ARM64 真机已验证清单中固定的 JRE8、17、21、25 制品。17/21/25 通过 CheckJNI 严格模式的 smoke 与 coverage；8 通过严格 smoke 和正常 JIT 模式的完整 coverage，但其 VM 的 CheckJNI 数组提交路径存在独立可复现的崩溃，不能标为严格 coverage 通过。生产适配器已实现六个装载接口族；真实桌面 JRE8、11、17、21、25 通过装载契约测试，传输使用主机自身的 libc。其他 Android 制品、其他厂商实现和 OpenJ9 需要另外确认。

| 运行验证 | 接口族 | 验证范围 |
| --- | --- | --- |
| Temurin 8u512 | CLASSLOADER_8 | 31 项装载/回收断言与 3 项恢复失败断言 |
| 本机 OpenJDK 11.0.25 | CLASSLOADER_10_THROW | 同上 |
| 本机 OpenJDK 17.0.21.0.3 | NATIVE_LIBRARIES_18 | 同上 |
| Temurin 21.0.13 / 25.0.5 | NATIVE_LIBRARIES_19 | 各自同上 |
| 固定 Android JRE8 / 17 / 21 / 25 | CLASSLOADER_8 / NATIVE_LIBRARIES_15 / NATIVE_LIBRARIES_19 | ARM64 双 libc、142 项 Java/JNI、26 项 loader、SQLite/zstd、数据重开与完整退出；8 的严格模式限制见下 |

桌面固定归档见 `tests/config/desktop-jres.json`，本地 VM 必须显式传入目录和接口族。每次结果记录实际 VM 版本、libjava/libjvm 散列、结构化断言、CheckJNI 日志及无适配器的启动对照。未列入运行矩阵的接口族或 GA 基线不能仅凭源码审计标为运行通过。JDK28 只保留既有源码审计资料，本轮不实现或验证其运行支持。

基础 core、C loader 和公开 JNI 桥保持独立；本项目只转换 VM 私有装载边界。不要为不同 JRE 复制整个 glibc-worker。

## Android 制品识别与候选验证

`tests/config/android-jres.json` 固定 ID、ABI、归档散列、libjava/libjvm 路径及散列、接口族和测试启动策略。`GW_JRE_ARTIFACT` 显式选择；没有按主版本猜测、按当前文件自动生成可信 profile 或失败后尝试其他族的路径。提取缓存每次重新验证两个 ELF 的散列、64 位小端格式和机器架构；安装后再次验证实际文件。生产 install 只接受已驻留 libjava，并验证它关联的 JavaVM 就是当前 JNIEnv 的 VM，同时散列校验实际 VM 映像。

清单内的制品身份与运行结论分开记录，桌面 VM 与 Android VM 不能互相替代。ARM64 真机验证使用 Android 13 普通应用环境；覆盖测试包含 CheckJNI（严格组）、动态资源、Host/Guest 线程、重入、类注册交接、三次加载/卸载、SQLite/zstd、保存重开、Guest 停止及 VM 销毁。

| Android ARM64 制品 | 启动夹具 | 运行结论 |
| --- | --- | --- |
| Pojav17 ec28559，17-internal | 已固定的路径缓存修补 | 严格 smoke / coverage 均通过 |
| AngelAura8，1.8.0_482-internal | Pojav 目录布局 + JLI | 严格 smoke 通过；正常 JIT coverage 通过；严格 coverage 因 VM 的 JNI_COMMIT 缺陷失败 |
| AngelAura21，21.0.1-internal | Pojav 目录布局 + JLI | 严格 smoke / coverage 均通过 |
| AngelAura25，25.0.5-internal | 同上，另固定 NDK libc++ | 严格 smoke / coverage 均通过 |

制品、模式、APK/报告散列与反例摘要见 [验证记录](../tests/results/android-jres-arm64.json)。

运行结论只覆盖这些固定制品与上述模式，不代表所有同主版本 JRE 或完整 JNI 规范通过。公开桥在 JRE8 上最高声明 JNI1.8，并测试 GetModule 抛 UnsupportedOperationException；17/21/25 最高声明 JNI10，并测试 GetModule 正常返回。功能触达计数包含明确拒绝路径，不表示宿主支持全部槽。

AngelAura 测试应用将库复制到原始 JRE 目录并标为只读，按 Pojav 的预加载与 JLI_Launch 过程启动应用进程内的 HotSpot。来源固定于 `tests/config/pojav-startup.json`，没有引入完整启动器或修改其产品代码。直接执行私有目录 bin/java 在真机被拒绝，linker64 代理版本查询成功；该子进程探针与同进程的桥测试分别记录。ARM64 exec 子进程还需显式关闭堆指针标记，它不继承应用 manifest 的声明。具体启动条件见 [测试手册](../tests/README.md)。

切换 JRE 时清理旧受管文件、只读文件及空目录；JRE8 遗留的空 lib/ext 也会使 JRE17 拒绝启动。现有 JVM 宿主负责自己启动 JRE，只接入已运行 VM 的适配器，不应再次调用测试夹具创建 VM。JRE17 的私有偏移只用于固定制品，不自动猜测或写入其他制品。

### JRE8 CheckJNI 的明确限制

固定 ARM64 JRE8 开启 CheckJNI 时，GetBooleanArrayElements 返回 isCopy=true，JNI_COMMIT 后的最终释放触发 Scudo abort。同一份 VM、同一套 JNI C 用例在不加载 glibc-worker 的独立进程中复现；不开启 CheckJNI 则完成数据回写。可运行 `tests/scripts/test-array-commit.py` 复核，其 PASS 仅表示成功复现 VM 缺陷，结构化结果同时记录 strict_jni_supported=false。

所核对的 [OpenJDK8 维护快照](https://github.com/openjdk/jdk8u/blob/b661ee6ff840d6922aa7424a4d8f24760b21cf26/hotspot/src/share/vm/prims/jniCheck.cpp) 在 check_wrapped_array_release 中无条件释放 GuardedMemory 副本，包括 JNI_COMMIT；这与 [JNI_COMMIT 保留复制缓冲区的契约](https://docs.oracle.com/en/java/javase/24/docs/specs/jni/functions.html#releaseprimitivetypearrayelements-routines) 不符。项目没有跳过提交/最终释放断言，没有屏蔽崩溃，也没有以关闭 CheckJNI 后的通过替换严格组结果。此制品的正常模式验证为 go，严格数组检查模式为 no-go；需要严格模式的调用方应修复或更换 VM 后重验。

x86_64 制品使用独立的归档和散列，特别是 JRE8 的补丁版本与 ARM64 不同。清单包含它们以便模拟器复现，不能由 ARM64 通过推断其覆盖结果，也不能把 x86_64 的旧结果当作新制品的验证。

## OpenJDK GA 接口族

逐一核对 JDK8–27 的 20 个 GA 基线，并另外固定一个 JDK28 early-access 开发快照。主要装载/查找/卸载边界可归纳为以下五组；同组不表示整个实现或运行行为逐字相同。

| GA 版本 | load 边界 | 查符号与卸载 | 适配意义 |
| --- | --- | --- | --- |
| 8–9 | ClassLoader$NativeLibrary 的实例 `load(String, isBuiltin): void` | 实例 find；实例 unload，无显式 handle 参数 | 独立旧类布局及实例方法适配 |
| 10–14 | 同类的实例 `load0(String, isBuiltin): boolean` | findEntry；静态 unload 接收 handle | 加载返回值、方法名和卸载 ABI 已变化 |
| 15–17 | NativeLibraries 的静态 load，参数为 impl、name、isBuiltin、isJNI | 同类 findEntry0 接收 impl；unload 含 isJNI 和 handle | 当前固定 JRE17 的主要接口形态 |
| 18 | 在上一组 load 末尾增加 throwExceptionIfFail | 查找与卸载仍保持上一组形态 | 需要额外布尔参数及失败策略 |
| 19–27 | load 参数为 impl、name、isBuiltin、throwExceptionIfFail；移除 isJNI | 查找移到 NativeLibrary.findEntry0(handle, name)；unload 移除 isJNI | 两个 boolean 的语义、查找所属类和卸载原型均须更新 |

可核对的上游端点：[8](https://github.com/openjdk/jdk8u/blob/jdk8-b132/jdk/src/share/classes/java/lang/ClassLoader.java)、[10](https://github.com/openjdk/jdk/blob/jdk-10%2B46/src/java.base/share/classes/java/lang/ClassLoader.java)、[15](https://github.com/openjdk/jdk/blob/jdk-15%2B36/src/java.base/share/classes/jdk/internal/loader/NativeLibraries.java)、[17](https://github.com/openjdk/jdk/blob/jdk-17%2B35/src/java.base/share/classes/jdk/internal/loader/NativeLibraries.java)、[18](https://github.com/openjdk/jdk/blob/jdk-18%2B36/src/java.base/share/classes/jdk/internal/loader/NativeLibraries.java)、[19](https://github.com/openjdk/jdk/blob/jdk-19%2B36/src/java.base/share/native/libjava/NativeLibraries.c)、[25](https://github.com/openjdk/jdk/blob/jdk-25%2B36/src/java.base/share/native/libjava/NativeLibraries.c)。所有中间版本的文件、标签与散列见审计 manifest。

JDK17 与 JDK19 的 load 描述符都以 `String;ZZ)Z` 结束，第二个布尔参数却从 isJNI 变成失败时是否抛异常。因此“类名和描述符匹配”不足以证明语义兼容，不能仅依据参数数量选择适配器。

`handle:J`、`jniVersion:I`、`fromClass:Class` 这些核心字段在所查 GA 源码中持续存在，但所属类、使用方式、卸载对象与同步实现有变化。字段稳定也不等于整套生命周期可直接复用。

## JDK25–28 的装载和 JNI 边界

| 基线 | 固定来源 | 相对 25 的装载入口 | 公开 JNI 表 |
| --- | --- | --- | --- |
| 25 GA | jdk-25+36 | 对照 | 对照 |
| 26 GA | jdk-26-ga 对应 commit 4408cd2a07a14243a58cd9d30813302bfbe81133 | 相同 | 函数槽和参数相同 |
| 27 GA | jdk-27-ga 对应 commit 815ff4dc327fe17f2433c7d115a5a503af10f3c4 | 相同 | 函数槽和参数相同 |
| 28 early-access | commit ce986f721b1d1e4d1503c50b8070a52f6f4e970a | 此快照相同 | 末尾新增 HasIdentity，增加 JNI_VERSION_28 |

四个基线的 `NativeLibraries.c` 文件逐字节一致，load/unload/find 的导出符号、参数及函数体相同；三个 JVM 装载 API 的原型也相同。`NativeLibrary.findEntry0`、库对象的 handle/jniVersion/fromClass 形态保持不变。因此这些基线无需新增装载接口族；这仍不是 Android 二进制或完整运行兼容性的证明。

Java 内部组织并非完全冻结：相对 25，后续源码为 AOT 初始化调整了 loadedLibraryNames 的 Holder 存放位置，并增加 clear 方法。这些没有改变当前钩子所需的 native 方法边界；如果未来适配器依赖其他私有字段或初始化顺序，需要另行审阅。

28 快照的公开 JNI 表在末尾增加 `HasIdentity(JNIEnv*, jobject)`，用于值类相关的 identity 判断；已有槽位及参数保持原顺序。当前桥尚未实现此新槽或 JNI28 版本协商，新模组使用它时必须明确识别能力缺失，不能因为装载 ABI 相同就宣称支持。

28 快照的版本配置标记为 ea，目标日期为 2027-03-23。这是开发状态，后续提交及最终 GA 仍需重新核对。可查证的源码：[26 装载](https://github.com/openjdk/jdk/blob/jdk-26-ga/src/java.base/share/native/libjava/NativeLibraries.c)、[27 装载](https://github.com/openjdk/jdk/blob/jdk-27-ga/src/java.base/share/native/libjava/NativeLibraries.c)、[28 固定快照](https://github.com/openjdk/jdk/blob/ce986f721b1d1e4d1503c50b8070a52f6f4e970a/src/java.base/share/native/libjava/NativeLibraries.c)、[28 JNI 表](https://github.com/openjdk/jdk/blob/ce986f721b1d1e4d1503c50b8070a52f6f4e970a/src/java.base/share/native/include/jni.h)、[28 版本配置](https://github.com/openjdk/jdk/blob/ce986f721b1d1e4d1503c50b8070a52f6f4e970a/make/conf/version-numbers.conf)。

## 相对稳定的底层

所查版本持续通过 JVM_LoadLibrary/JVM_FindLibraryEntry/JVM_UnloadLibrary 调用平台 loader，并在 libjava 层处理 JNI_OnLoad/OnUnload、JNI 版本验证和加载失败清理。整体机制变化少于上层组织方式。

JVM_LoadLibrary 本身也不是不变 ABI：8–17 GA 接收 name，18–27 GA 增加 throwException 布尔参数；所查 28 开发快照仍保留该原型。源码对比支持“主流程长期稳定”，不支持把某个私有 C 原型当作永久标准。现有适配器通过 JNI native 方法注册接入，没有直接把不同版本的 JVM_LoadLibrary 地址强转为同一个函数指针。

## 维护分支与回移植

审计另外固定了 8u、11u、17u、21u、25u 的五个维护分支 commit；这是五个具体快照，不是所有小版本的穷举。

- 所查 8u 的三个主要方法仍属于 8–9 形态。
- 所查 [11u](https://github.com/openjdk/jdk11u/blob/d001c53e35a816f26718845b5d26a877878a7f5d/src/java.base/share/classes/java/lang/ClassLoader.java) 已给 load0 增加 throwExceptionIfFail，与 11 GA 的参数不同。
- 所查 [17u](https://github.com/openjdk/jdk17u/blob/14a8c6288c963d6d61f12227375398d8abafc709/src/java.base/share/classes/jdk/internal/loader/NativeLibraries.java) 的 load 有三个 boolean，接近 18 GA 形态；固定测试 JRE17 仍是旧的两个 boolean。
- 所查 21u/25u 的三个主要 native 入口保持 19–25 GA 形态。

因此不能只按 major version 分派。版本升级不必然破坏 ABI，但回移植确实能在同一大版本内改变私有入口。二进制 SHA256 改变只证明制品身份不同；当前拒绝新散列是审阅门槛，不是该制品已经被证明不兼容。

## 适配器扩展规则

接口族、制品身份和运行验证分开处理：接口族定义精确类/字段/原型、布尔语义和生命周期；v2 profile 固定审阅后的 libjava/libjvm 配对制品；只有实际测试完成才进入对应范围的支持清单。调用方通过构建 profile 或 `gw_jvm_jni_set_profile` 选择，未知族、无效散列、字段或方法不匹配均拒绝。

注册前完整解析并核对全部符号、方法和字段；未知形态拒绝，不做有副作用的试调用。验证失败不能 fallback 为 bionic 加载 GNU DSO。保留事务式注册/回滚；无法恢复时丢弃进程。不要为了跨版本而删除散列检查后盲目放行。

旧的 `GW_JRE17_*` 构建兼容输入与 JRE17 专用名称已删除。接入必须使用 `GW_JVM_PROFILE_FAMILY` 或 v2 profile 的 family 字段，不能只按参数数量或 JRE 主版本选择。各族的准确值及生命周期见 [适配器接口](ADAPTER.md)。

## 装载之外的必要检查

- 公开 JNI 桥覆盖 JNI1.6 基础槽加 GetModule，最高声明 JNI10。启动时协商宿主与桥的能力，Guest GetVersion/GetEnv/AttachCurrentThread 以及 JNI_OnLoad 接受值都不能超过协商结果。JRE8 上最高 JNI1.8；较新宿主最高 JNI10；JNI19/21/24/25 的新增能力请求会被拒绝。旧宿主的 GetModule 调用抛 UnsupportedOperationException，不能越过函数表读取。仅新增 loader profile 不完成所有新 JNI 槽的兼容工作。
- JDK24/25 的 [Runtime](https://github.com/openjdk/jdk/blob/jdk-24%2B36/src/java.base/share/classes/java/lang/Runtime.java) 在 load/loadLibrary 前增加原生访问检查；接入应保留目标 VM 的模块策略。
- 固定 Android JRE 测试宿主包含私有启动修补；不能将它视为所有 Android JRE 的通用启动方案。
- 每个支持制品至少验证 JNI_OnLoad 成功/拒绝、版本失败、符号和重载、不同 ClassLoader 所有权、回调与重入、并发/TLS、OnUnload、引用清理、Guest 停止和 VM 销毁。真实库与模组再验证数据和保存退出。

## 可复现审计

`tests/config/openjdk-loader-sources.json` 固定 129 个上游文件的仓库、GA 标签或完整 commit、路径与 SHA256，包含 26 个源码快照：20 个 GA、5 个维护分支和 1 个 JDK28 early-access。审计工具只下载已固定文件并提取结构化边界，不运行这些 VM：

```sh
python3 jvm-jni/tests/scripts/audit-openjdk-loader.py
python3 jvm-jni/tests/scripts/audit-openjdk-loader.py --offline
```

下载缓存和 `loader-boundaries.json` 输出到本项目 `build/jre-loader-audit/`。结果包含 Java native 声明、C 导出原型、函数体 token 散列及 JVM 层原型。token 散列用于定位需人工审阅的差异，不是 ABI 或正确性证明。更新 profile 时应保留此源码审计，并按 [测试手册](../tests/README.md) 完成真实运行验证。
