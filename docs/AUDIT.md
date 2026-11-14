# 实现限制与待审计问题

审计基线：2026-11-14 的工作区源码。范围为 `core`、`bionic-dlopen`、`android-jni`、`jvm-jni` 及组合构建配置，不包含外部 Pojav 工程。

本文用于逐项审阅实现边界和失败语义，不将现有常量视为已经批准的长期设计。条目未勾选表示尚未完成审计决策或相应验证；记录源码问题不代表已经修复。除明确标注的构建选项外，数值上限均不能在运行时调整。共享协议变化必须同时重编和部署 Host、Guest。

## 审阅入口

- [ ] AUD-01：审定 32 个模块槽、512 个跳板、16,384 个引用/ID 槽、256 个注册类记录的容量及扩容方式。
- [ ] AUD-02：统一描述符长度检查，消除反射、注册、命令路径中的静默截断。
- [ ] AUD-03：统一资源耗尽和重入超限的失败语义，避免仅返回 NULL/零值而丢失明确错误。
- [ ] AUD-04：在启动栈写入前校验 argc、参数和环境字符串所需空间。
- [ ] AUD-05：处理 RPC 序号的有符号溢出及引用/ID 代数回绕。
- [ ] AUD-06：验证 glibc 补丁关闭 robust-list 登记后的互斥量语义，明确支持契约。
- [ ] AUD-07：审定 JNI 上下文容量、重入深度、注册所有权和跨依赖库的归属规则。
- [ ] AUD-08：审定卡死时的 deadline、取消、关闭及进程处置策略。
- [ ] AUD-09：审定 runner 的状态标记和制品隔离；当前 ELF 修改已完成真机回归，失败尝试不能沿用旧 PASS。
- [ ] AUD-10：按具体 Android JRE 制品扩展验证，不把桌面适配器测试当作 Android 双 libc 通过。
- [ ] AUD-11：决定是否继续保留 JRE17 旧名称兼容入口；它们不承载另一套运行时实现。

## 1. 装载与 JNI 资源

| 项目 | 当前限制 | 作用域与耗尽行为 | 审计项 |
| --- | --- | --- | --- |
| Guest Runtime / 目标 VM | 每进程一套 | 单例；停止后不能重新配置并启动另一套 | AUD-08 |
| 顶层 Guest 模块 | 32 槽 | 所有模块共享；加载中、已加载、关闭中都占槽；耗尽返回 `GW_E_CAPACITY` | AUD-01 |
| JNI 函数跳板 | 默认 C 后端共享 512 个；Zig 实验后端按需增长 | 按模块、Guest 函数地址和描述符复用；分配失败使查找或注册失败；Zig 选择同时覆盖引用/ID 和注册类存储 | AUD-01、AUD-03 |
| JNI 引用与 ID | C 对照 16,384 槽；Zig 按需增长 | local/global/weak 引用与 methodID/fieldID 缓存共用；耗尽返回 NULL 并累计桥错误，未统一抛出 OOM | AUD-01、AUD-03 |
| 注册类所有权记录 | C 对照 256 条；Zig 按需增长 | 所有模块共享；无可用记录使 `RegisterNatives` 失败 | AUD-01、AUD-07 |
| 同一 Java 类的注册所有者 | 一个 Guest 模块 | 其他模块不能同时持有该类的 native 注册 | AUD-07 |
| 单次 `RegisterNatives` | 最多 64 个方法 | 超过返回 `JNI_ERR`；分批仍受共享跳板和注册记录容量约束 | AUD-01、AUD-03 |
| JNI 上下文 | 默认及当前 ARM64 构建为 16 个 | 包含 owner 上下文，额外剩 15 个；Host 调用线程与 Guest 附着线程共享 | AUD-07 |
| 上下文构建范围 | `GW_CONTEXTS=2..256` | 组合脚本允许的构建范围；不是运行时热调整参数 | AUD-07 |
| 调用协议深度 | 12 个槽，索引 0..11 | Host / Guest 达到边界后，部分路径累计错误并返回零值；具体可重入层数受双向调用路径影响 | AUD-03、AUD-07 |
| native 显式参数 | 最多 32 个 | 不含隐含的 `JNIEnv*` 和对象/类参数；每个 long/double 仍只占一个 `jvalue` 参数项 | AUD-07 |

32 个槽限制的是本项目管理的顶层库，不是进程中所有 `.so` 的数量：

- 成功卸载或加载失败回滚后可复用槽位，不是整个进程累计只能加载 32 次。
- `DT_NEEDED` 自动加载的依赖及 Guest 内部自行 `dlopen` 的库不逐个占用这些槽位。
- Guest 内部自行加载的库不会自动得到独立的桥接模块句柄和完整生命周期管理。
- Host 的普通 bionic 库不占 Guest 模块槽；一个模组也可能加载多个顶层 JNI 库。
- `UnregisterNatives` 当前不会单独清空模块的所有跳板和注册所有权记录；主要清理点是模块卸载。不能把 512 简化为“当前正在注册的 Java 方法数”。
- 不经过 JNI 桥的 Guest pthread 不逐个占用 JNI 上下文；线程需要附着桥时才受上下文容量限制。它不是所有 Guest 线程的总数上限。

源码位置：

- [module_id.h](../bionic-dlopen/include/module_id.h)：`FB_MODULES`。
- [loader.c](../bionic-dlopen/src/loader.c)：`reserve`、`gw_dlopen_configure`、`gw_dlopen_stop`。
- [bindings.c](../android-jni/src/host/bindings.c)：`registrations`、`own_registration`、`fh_register`、`release_bindings`；[binding_table.c](../android-jni/src/host/binding_table.c) 保留 512 槽控制后端；[Zig 试点](../android-jni/docs/ZIG_BINDINGS.md) 说明动态后端边界。
- [references.c](../android-jni/src/host/references.c)：`insert_handle`、`fh_add_ref`、`fh_add_id`；[槽位契约](../android-jni/src/host/slot_store.h)。
- [full_protocol.h](../android-jni/src/common/full_protocol.h)：`FB_CONTEXTS`、`FB_DEPTH`、`FB_ARGS`。
- [build-runtime.sh](../scripts/build-runtime.sh)：`GW_CONTEXTS` 的默认值和范围校验。

## 2. 长度边界与截断

以下长度按 C 缓冲区中的字节计，不按 Java 字符数计。

| 项目 | 当前有效上限 | 当前失败语义 |
| --- | --- | --- |
| 请求路径、解析后的映像路径、libjava 路径 | 4095 字节正文 + 终止符 | 相关入口通常明确拒绝超长路径 |
| JNI 导出符号名 | 255 字节正文 + 终止符 | 后端拒绝超长查找 |
| JNI 方法描述符 | 255 字节正文 + 终止符 | `find` 入口明确拒绝；部分反射、注册和命令路径仍用 `snprintf` 截断，尚未统一 |
| Guest 附着线程名 | 63 字节正文 + 终止符 | 超长截断 |
| 错误和诊断消息 | 511 字节正文 + 终止符 | 超长截断，可能丢失诊断细节 |
| JVM profile 名称 | 127 字节正文 + 终止符 | 必须在固定数组内终止 |
| profile 的 libjava SHA256 | 64 个小写十六进制字符 + 终止符 | 长度或字符格式不符拒绝 |

AUD-02 的问题并未因公开 `find` 入口有长度检查而消失：

- `fh_describe` 将反射得到的描述符写入固定 256 字节缓冲区。
- `fh_add_id` 与 binding 准备现已先检查长度再完整复制；这不代表反射与协议路径都已完成审阅。
- `fh_command` 也会向固定协议缓冲区复制字符串。
- 必须分别核对各条路径；不能把截断后的描述符继续当作完整类型信息使用。

源码位置：[loader_types.h](../bionic-dlopen/include/loader_types.h)、[loader_backend.c](../android-jni/src/host/loader_backend.c) 的 `valid_descriptor`、[references.c](../android-jni/src/host/references.c)、[bindings.c](../android-jni/src/host/bindings.c)、[full_host.c](../android-jni/src/host/full_host.c) 的 `fh_command`、[jvm_adapter.h](../jvm-jni/include/jvm_adapter.h)。

## 3. 自动 ELF 分类边界

| 项目 | 当前上限或要求 | 不满足时 |
| --- | --- | --- |
| ELF 类型 | 当前架构的 ELF64、小端、`ET_DYN` | `GW_E_IMAGE` |
| Program headers | 128 个 | `GW_E_CAPACITY` |
| Dynamic entries | 512 个，包括段内填充和终止项 | `GW_E_CAPACITY` |
| 被检查的依赖名、版本名 | 255 字节正文 + 终止符 | 超长为 `GW_E_CAPACITY`；表内未终止等损坏为 `GW_E_IMAGE` |
| 版本依赖组 | 64 组 | `GW_E_CAPACITY` |
| 每组版本条目 | 256 个 | `GW_E_CAPACITY` |
| 截断、非法元数据地址、错误链表或缺少终止项 | 必须通过检查 | `GW_E_IMAGE` |

当前实现已区分三种结果：`GW_ELF_GLIBC`、`GW_ELF_UNMARKED`、`GW_ELF_ERROR`。只有成功解析支持的元数据且未发现 GNU 标记，AUTO 才返回 `GW_PASS`；损坏、不支持和资源超限都终止该次装载，不能将“无法判断”解释成“非 glibc”。显式 `GW_OPEN_GUEST` 跳过自动分类，由调用方确认 GNU 域。

分类器不是完整 ELF 验证器，不代替实际 GNU/bionic loader 的重定位与符号检查。这些上限目前仍保留，是否扩大或改用其他数据结构由 AUD-01 / AUD-09 审阅，不能仅改回模糊的布尔结果。

源码位置：[elf_route.h](../bionic-dlopen/src/elf_route.h)、[elf_route.c](../bionic-dlopen/src/elf_route.c)、[loader.c](../bionic-dlopen/src/loader.c) 的 `loader_open`。测试：[elf_route.py](../bionic-dlopen/tests/elf_route.py)、[loader_contract.c](../bionic-dlopen/tests/loader_contract.c)。

状态：49 项 ASan/UBSan 元数据检查和纯 C 装载检查通过；最新分类变更的 Android 真机回归未完成，见第 9 节。

## 4. 数组副本缓存

| 项目 | 当前值 | 资源不满足时 |
| --- | --- | --- |
| 缓存槽数 | 8 | 回退原 JNI 数组接口 |
| 单份副本启用范围 | 2..8 MiB，含端点 | 范围外回退 |
| 缓存容量预算 | 32 MiB | 无可用预算时回退 |
| 缓存分配失败 | 允许发生 | 回退 |

这些是优化缓存的边界，不是数组总大小、进程内存或可传输数据总量的上限。8 MiB 以上的数组不因此被禁止。`JNI_COMMIT` 保持副本占用，直至最终释放；缓存槽耗尽仍应保持原 JNI 语义。

源码位置：[array_copy.c](../android-jni/src/host/array_copy.c)。契约：[ARRAY_COPY_CACHE.md](../android-jni/docs/ARRAY_COPY_CACHE.md)。

## 5. core 启动器

本节针对 bootstrap 启动一个匹配的 GNU loader；不应把它的 64 个 program headers 与 DSO 自动分类的 128 个混为一项。

| 项目 | 当前限制 | 审计说明 |
| --- | --- | --- |
| 架构 | ARM64、x86_64 | 没有 ARM32 / x86_32 实现 |
| 启动 loader 的 program headers | 64 个 | 超出会在启动检查中失败 |
| loader 每段地址和大小 | `p_vaddr`、`p_memsz` 分别小于 16 MiB | 不是所有 Guest DSO 的大小限制 |
| loader 布局 | 加载段按地址排列，页取整后不重叠；禁止 RWX 段 | 需要与所选 loader 制品配合 |
| 启动参数保存数组 | 16 项 | `av[16]`；当前未先校验 argc，且后续使用 `av[0]`，必须处理 0 项和超量参数 |
| 启动环境 | 当前只构造 2 项 | `LC_ALL=C`、关闭 rseq 的 `GLIBC_TUNABLES`；不自动继承完整 Host 环境 |
| 启动栈空间 | 调用方提供 | 当前在复制后才检查 4 KiB 余量，缺少写入前的完整容量检查 |
| 启动失败 | 部分路径直接 `exit(1)` | 不能当作返回错误后可恢复重试的接口 |
| GNU 映射生命周期 | 保留至进程退出 | 尚无完整销毁映射并重新创建 runtime 的通用 API |

AUD-04 中的 argc 和栈检查是待修缺陷，不是靠调用者遵守文档就应永久保留的设计。

源码位置：[arch.h](../core/include/arch.h)、[loader_image.c](../core/src/bootstrap/loader_image.c) 的 `gw_bootstrap_map`、`copy_string`、`gw_bootstrap_stack`；边界说明：[core README](../core/README.md)。

## 6. JNI、所有权和生命周期

| 项目 | 当前边界 |
| --- | --- |
| Guest JNI 版本 | 最高 JNI10，并受真实 Host 能力限制；JRE8 下最高 JNI1.8 |
| 更新 JNI 槽 | JNI19 及以后新增能力未实现，对应版本请求拒绝 |
| `GetModule` | 低于 JNI9 的 Host 上抛不支持异常，不能读取不存在的表项 |
| 新附着线程的 ThreadGroup | 只支持 NULL；已有附着上下文的返回路径另行处理 |
| Guest `DestroyJavaVM` | 返回 `JNI_ERR`，由 Host 管理真实 VM 生命周期 |
| Guest `DetachCurrentThread` | 不能分离 Host 来源上下文，不能在嵌套调用中分离 |
| JNI 重载推断 | 多个重载只提供一个短 JNI 符号时，不能唯一确定描述符 |
| 注册函数的自动归属 | 无当前模块上下文时，主要按已记录顶层 DSO 的 `link_map` 推断；依赖库函数归属需要专项审阅 |
| 同类注册所有权 | 一个类的 native 注册只归属一个 Guest 模块；`UnregisterNatives` 后的所有权释放策略仍需审定 |
| 停止 | 有加载中、活跃或关闭中模块时拒绝停止 |
| 自身回调中关闭模块 | 返回 BUSY，避免等待自身 |
| 后台线程 | 模组必须停止并 join；桥不会自动发现任意后台线程或缓存函数指针的借用 |
| 等待和取消 | 无通用 deadline / 超时取消协议；对端卡住可能无限等待 |
| 跨 libc 对象 | 不可自由混用 allocator、`FILE`、pthread 对象、C++ 对象及异常；Guest 内部使用 C++ 不因此被禁止 |
| 进程故障隔离 | 同进程执行；Guest 原生崩溃和进程级 `exit` 没有进程隔离 |
| Host/Guest 协议 | 配置必须匹配并成套部署；当前 JNI 能力握手要求 Guest 确认后才 ready |
| runtime 组合 | 模块化 DSOs 与兼容单 DSO 二选一，不能在同一进程重复装入两套单例状态 |

源码与契约位置：[jni_capabilities.h](../android-jni/src/common/jni_capabilities.h)、[full_guest.c](../android-jni/src/guest/full_guest.c)、[guest_loader.c](../android-jni/src/guest/guest_loader.c)、[full_host.c](../android-jni/src/host/full_host.c)、[bindings.c](../android-jni/src/host/bindings.c)、[JvmAdapter.java](../jvm-jni/src/java/dev/glibc/worker/JvmAdapter.java)、[LOADER_BACKEND.md](../android-jni/docs/LOADER_BACKEND.md)。

## 7. 运行时制品、部署和适配

| 项目 | 当前边界 |
| --- | --- |
| glibc 制品 | 固定构建经过补丁的 glibc 2.44，loader/libc 必须配对部署 |
| 补丁应用验证 | 构建脚本隔离父级 Git 环境，核对反向补丁；跳过补丁或损坏缓存不能当作成功，见 [RUNTIME_PATCHES](../core/docs/RUNTIME_PATCHES.md) |
| clone3 | 当前 glibc 补丁改走旧 clone 路径 |
| rseq | 当前补丁和启动环境关闭登记 |
| robust-list | 当前补丁移除了登记；不能承诺 robust mutex 的 owner-death 恢复语义，需专项实验 |
| 依赖闭包 | 不自动下载或补齐完整 rootfs；必须部署模组需要的 GNU 依赖 |
| 默认兼容 stage | 主要部署 loader/libc/libpthread/libm；不能将其等同于包含所有第三方库的 rootfs |
| 映像策略 | 默认按安装目录或内容散列目录解析；目录外请求需匹配同字节已安装映像，或显式开启直接路径模式 |
| 目录映射条目的名字 | `lib[A-Za-z0-9_.-]+\.so`，并检查规范化后的父目录和内容一致性 |
| Android 执行权限 | 直接路径模式不绕过 Android 的路径、可执行映射及系统调用策略 |
| JVM 私有接口 | 选择审阅后的接口族并匹配 libjava SHA256；字段、方法或原生符号不符时拒绝 |
| profile 生命周期 | 只在未安装、无并发装卸载和调用时配置；已安装时不能更换 |
| 安装失败恢复 | 尝试恢复原钩子；再次失败时保留状态并要求丢弃进程 |
| Android 构建基线 | 当前组合构建 API29，测试应用 minSdk29 / targetSdk36；这不是所有更低 Android 版本已被证明不能支持的结论 |
| 特定 JRE 的启动 | 测试宿主包含固定制品的私有启动修补，不能当作所有 Android JRE 的通用启动接口 |
| ART | 公开 JNI 层与 JRE 私有层已分离，但完整 ART 桥未验证 |
| fork、任意信号处理与完整销毁重建 | 没有完整兼容性验证，不因普通 JNI 测试通过而自动成立 |

robust mutex 条目属于从现有补丁得出的能力缺口判断，尚无专项 owner-death 测试结论。AUD-06 应检查支持与拒绝行为，而非仅验证普通 mutex 的加锁解锁。

旧的 [jre17_adapter.h](../jvm-jni/include/jre17_adapter.h) 仅声明转发到通用适配器的兼容名称，不再承载 JRE17 专用实现。维护中的 SDK 消费者使用 [jvm_adapter.h](../jvm-jni/include/jvm_adapter.h)。AUD-11 应明确是否需要旧源码/二进制接口兼容；若删除，应同步清理转发符号、安装清单和相关文档。

源码位置：[build-glibc.sh](../core/scripts/build-glibc.sh)、[Termux NPTL 补丁](../core/patches/termux/set-nptl-syscalls.patch)、[clone3 补丁](../core/patches/termux/disable-clone3-glibc244.patch)、[package-runtime.sh](../android-jni/scripts/package-runtime.sh)、[GuestImages.java](../android-jni/src/java/dev/glibc/worker/GuestImages.java)、[profiles.c](../jvm-jni/src/native/profiles.c)、[ADAPTER.md](../jvm-jni/docs/ADAPTER.md)。

## 8. 等待、诊断和长期计数

| 项目 | 当前值或风险 |
| --- | --- |
| 主动等待预算 | 默认 20 μs，组合脚本允许 0..1 ms；不是 RPC 延迟上限，空闲路径使用 futex |
| RPC profiling | 默认关闭；开启后约 1/64 采样、64 个直方图桶，保留 16 条最差交接记录 |
| 通知过滤 / ready polling | 当前默认均关闭；开启时 Host、Guest 必须使用匹配配置 |
| 模块代数 | 32 位 unsigned；每槽达到 `UINT_MAX` 后不再复用，避免回绕 |
| 引用 / ID 代数 | 32 位，达到 `UINT32_MAX` 后永久退役槽位，C/Zig 均不回绕；token 仅在当前 store 生命周期内有效 |
| RPC 请求序号 | 有符号 32 位 `seq + 1`；到 `INT_MAX` 后存在 C 有符号溢出风险，缺少明确回绕策略 |
| 统计快照 | 实时原子采样，不是事务视图；采样尾部和计数器不能当作精确的全量性能数据 |

引用槽位通过缩小配置中的代数上限验证耗尽退役，不以运行 2³² 次替代。RPC 请求序号问题仍待处理；不能把计数宽度当作已验证的安全运行次数。

源码位置：[loader.c](../bionic-dlopen/src/loader.c) 的 `reserve`、[references.c](../android-jni/src/host/references.c) 的引用分配路径及 [slot_store.zig](../android-jni/src/host/slot_store.zig)、[full_host.c](../android-jni/src/host/full_host.c) 的 `fh_command`、[full_guest.c](../android-jni/src/guest/full_guest.c) 的 `fg_rpc`、[rpc_metrics.h](../core/include/rpc_metrics.h)、[rpc_handoff.h](../core/include/rpc_handoff.h)、[build-runtime.sh](../scripts/build-runtime.sh)。

## 9. 已有证据与未完成验证

当前 ARM64 组合构建配置：16 contexts、profiling 关闭、主动等待 20 μs、通知过滤关闭、ready polling 关闭。可核对本地 [build.properties](../jvm-jni/build/tests/runtime/arm64-v8a/build.properties)。这些值不代表外部 Pojav 历史测试包的配置。

| 验证 | 已有证据 | 尚不能证明 |
| --- | --- | --- |
| 桌面 JRE8/11/17/21/25 适配器 | 每个 VM 27 项正常契约断言及 3 项恢复失败断言通过 | 所有 Android JRE 制品、双 libc、所有新增 JNI 能力均兼容 |
| Android 固定 JRE17 | 当前 C / Zig 两种后端均通过：142 项 Java/JNI、26 项装载、234/235 槽功能触达；Guest 退出与 VM 销毁完成 | 完整 JNI 规范、所有源码分支或其他 Android JRE 制品 |
| ELF 分类三态与边界 | 49 项 ASan/UBSan 检查、C 装载检查和当前 Android 组合回归通过 | 真实 GNU loader 的完整 ELF 验证 |
| Zig Host 资源管理 | 桌面 8,192 个存储 binding、32,768 个引用、1,024 个注册类；Android 三轮各 1,024 个方法、20,000 个引用和 320 个注册类通过，最终 closure/引用为 0 | 默认后端已切换、所有资源上限已取消、任意并发卸载安全或性能提升 |
| runtime 补丁流程 | 8 项回归检查；修复静默跳过补丁后，C 与 Zig 真机组合均通过 | robust mutex owner-death 恢复语义 |

本地结果在各项目 `build/`，不是源代码发布包的必备文件：

- [桌面适配器结果](../jvm-jni/build/adapter-tests/summary.json)。
- [ELF 边界结果](../bionic-dlopen/build/test-results/zig-pilot-elf-route.json)。
- [C 对照真机结果](../jvm-jni/build/zig-resources/coverage-c-layout/summary.json) 与 [Zig 真机结果](../jvm-jni/build/zig-resources/coverage-zig-layout/summary.json)。
- [Host 资源管理验证汇总](../android-jni/build/zig-resources/summary.json) 包含桌面、真机及 runtime 制品散列。

历史 Android 测试目录可能仍保留较早 APK 的 PASS summary，失败尝试未必覆盖它。审阅应核对报告中的 APK SHA256；本轮 C / Zig 结果使用不同输出目录及固定 APK，不能只检查目录中是否存在 PASS。

| 记录 | SHA256 |
| --- | --- |
| 当前 C 对照 PASS APK | `71f538414b08c64106e7eb625a03044c4e1d45f7da9ba396d351d1e082e1f176` |
| 当前 Zig 扩容 PASS APK | `e7a3b41ebf8071c85c14327cce77937021237c6ba2fabf7ba248d75b178a4b97` |

- [ ] AUD-09 补充：runner 每轮应明确标记正在运行、失败或完成，并隔离旧 summary，避免失败尝试沿用旧 PASS。

原始兼容性和复现命令继续维护在各项目契约及 [测试手册](../jvm-jni/tests/README.md) 中。修复本清单条目时，应同步更新对应状态、限制值和验证范围，不将源码检查或单项测试自动升级为完整兼容性声明。
