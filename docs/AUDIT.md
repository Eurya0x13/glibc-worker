# 实现限制与待审计问题

审计基线：2026-11-14 的工作区源码。范围为 `core`、`bionic-dlopen`、`android-jni`、`jvm-jni` 及组合构建配置，不包含外部 Pojav 工程。

本文用于逐项审阅实现边界和失败语义，不将现有常量视为已经批准的长期设计。条目未勾选表示尚未完成审计决策或相应验证；记录源码问题不代表已经修复。除明确标注的构建选项外，数值上限均不能在运行时调整。共享协议变化必须同时重编和部署 Host、Guest。

## 审阅入口

- [x] AUD-01：模块固定 64 槽；binding、引用/ID、注册类记录初始容量为 512 / 16,384 / 256，统一使用 Zig 稳定分块扩容；完成 C 对照后删除定容后端。
- [x] AUD-02：反射、ID、注册、查找和命令路径检查描述符长度后完整复制；超长明确拒绝。
- [x] AUD-03：资源/调用帧扩容失败使用明确异常或结构化错误；保留已有异常，回滚未发布资源，扩容失败仍可查询/清除异常及释放资源。
- [x] AUD-04：启动栈写入前校验 argc、地址、参数、固定环境和完整帧空间；保护页/哨兵回归通过。
- [x] AUD-05：RPC 序号明确无溢出回绕；引用/ID 代数耗尽永久退役；现行 Zig 及已删除 C 对照均有边界验证。
- [x] AUD-06：复现关闭 robust-list 后的假成功与 owner-death 超时；robust mutex 初始化明确返回 ENOTSUP，普通锁/条件变量及真机组合回归通过。
- [x] AUD-07：上下文/重入帧稳定分块增长，保留线程身份与应急失败通道；完成类注册交接、严格依赖归属及执行 scope 关闭规则，默认/诊断真机组合通过。
- [x] AUD-08：每进程单 Runtime 绑定一个目标 VM，关闭后不重建；deadline 只作监督，不提前释放在途借用，无法确认安全时隔离并由宿主处置进程。生命周期/停滞保留主机验证及真实 Android JRE 覆盖、冒烟通过。
- [x] AUD-09：runner 独占结果目录，先归档旧制品再检查设备，原子记录本轮状态与 run_id，并核对安装 APK 散列。
- [x] AUD-10：v2 profile 校验 libjava/libjvm 配对及实际目标 VM，按具体 Android 制品记录验证；固定 ARM64 JRE17/21/25 严格覆盖通过，JRE8 正常模式完整通过；8 的 CheckJNI 数组提交崩溃已独立复现，不列为严格覆盖通过。
- [x] AUD-11：删除 JRE17 旧名称、转发符号、Java 包装类和构建宏兼容；SDK 消费者改用通用名称。
- [x] AUD-12：清理结果不确定时永久隔离桥、保留可能仍被使用的资源，由宿主丢弃进程；不自动杀应用。

AUD-03 的错误映射和故障回滚保持有效；AUD-07 将原固定深度/上下文耗尽替换为动态增长，扩容失败继续保留异常、撤回未发布资源并使用独立应急 RPC。AUD-07 的线程、类交接、依赖归属及容量规则见 [JNI 交互契约](../android-jni/docs/CONTEXTS_AND_OWNERSHIP.md)。AUD-08 的决策为不提供任意 native 的通用超时返回或强制取消：期限由宿主监督，真实完成/取消确认前保留在途资源，不能将隔离等同于安全撤销。单实例、关闭与处置规则见 [生命周期契约](../android-jni/docs/LIFECYCLE.md)。

## 1. 装载与 JNI 资源

| 项目 | 当前限制 | 作用域与耗尽行为 | 审计项 |
| --- | --- | --- | --- |
| Guest Runtime / 目标 VM | 每进程一套桥绑定一个选定目标 VM | 多模块/ClassLoader 共用；STOPPED/隔离均不可重建；core 不管理 JavaVM | AUD-08 |
| 顶层 Guest 模块 | 固定 64 槽 | 所有模块共享；加载中、已加载、关闭中都占槽；耗尽返回 `GW_E_CAPACITY` | AUD-01 |
| JNI 函数跳板 | 初始全局共享 512 槽，按 64 槽增长 | 按模块、Guest 函数地址和描述符复用；初始预留不创建 closure；分配失败使查找返回 `GW_E_CAPACITY`；JNI 注册返回 `JNI_ERR` 并保留或抛出 OOM | AUD-01、AUD-03 |
| JNI 引用与 ID | 初始共用 16,384 槽，按 64 槽增长 | local/global/weak 引用与 methodID/fieldID 缓存共用；分配失败返回 NULL，保留已有异常或抛出 `OutOfMemoryError`；撤销本次新建的 VM 引用 | AUD-01、AUD-03 |
| 注册类所有权记录 | 初始共用 256 槽，按 64 槽增长 | 所有模块共享；分配失败使 `RegisterNatives` 返回 `JNI_ERR` 并保留或抛出 OOM | AUD-01、AUD-07 |
| 同一 Java 类的注册所有者 | 一个 Guest 模块 | 其他模块不能同时持有该类的 native 注册 | AUD-07 |
| 单次 `RegisterNatives` | 最多 64 个方法 | 非法数量返回 `JNI_ERR` 并抛 `IllegalArgumentException`；分批仍受共享跳板和注册记录容量约束 | AUD-01、AUD-03 |
| JNI 上下文 | 默认初始 16 个，按 16 项动态增长 | 包含 owner；Host/Guest 共享目录、保留线程绑定；系统资源或索引边界不足分别为 OOM / `JNI_ENOMEM`，不等待其他绑定归还 | AUD-07 |
| 上下文初始容量构建范围 | `GW_CONTEXTS=2..256` | 仅配置初始内联容量，不是运行时最大上下文数 | AUD-07 |
| 调用协议深度 | 初始 12 层，按 16 层动态增长；独立应急 RPC | 分配/索引边界不足抛 OOM；应急通道不依赖扩容，并禁止在应急处理内再次发 Guest 命令；VM/线程栈仍是实际边界 | AUD-03、AUD-07 |
| native 显式参数 | 最多 32 个 | 不含隐含的 `JNIEnv*` 和对象/类参数；每个 long/double 仍只占一个 `jvalue` 参数项 | AUD-07 |

64 个槽限制的是本项目管理的顶层库，不是进程中所有 `.so` 的数量：

- 成功卸载或安全完成加载失败回滚后可复用槽位，不是整个进程累计只能加载 64 次；quarantine 时保留出错事务槽位。
- `DT_NEEDED` 自动加载的依赖及 Guest 内部自行 `dlopen` 的库不逐个占用这些槽位。
- Guest 内部自行加载的库不会自动得到独立的桥接模块句柄和完整生命周期管理。
- Host 的普通 bionic 库不占 Guest 模块槽；一个模组也可能加载多个顶层 JNI 库。
- 显式 `UnregisterNatives` 成功且无 pending exception 后释放类所有权，可交接给其他模块；旧模块跳板保留到卸载，防止影响其他类/在途调用。512 是初始存储容量，既不是活跃 Java 方法数，也不是硬上限。
- 不经过 JNI 桥的 Guest pthread 不占 JNI 上下文；附着桥才需要上下文/Host helper。动态目录不保证任意线程数成功，仍受 VM、线程栈、内存和系统限制。

源码位置：

- [module_id.h](../bionic-dlopen/include/module_id.h)：`FB_MODULES`。
- [loader.c](../bionic-dlopen/src/loader.c)：`reserve`、`gw_dlopen_configure`、`gw_dlopen_stop`。
- [bindings.c](../android-jni/src/host/bindings.c)：`registrations`、`own_registration`、`fh_register`、`release_bindings`；[binding_table.c](../android-jni/src/host/binding_table.c) 是 C ABI 薄层；[资源存储契约](../android-jni/docs/ZIG_BINDINGS.md) 说明唯一 Zig 后端及增长边界。
- [references.c](../android-jni/src/host/references.c)：`insert_handle`、`fh_add_ref`、`fh_add_id`；[槽位契约](../android-jni/src/host/slot_store.h)。
- [full_protocol.h](../android-jni/src/common/full_protocol.h)：`FB_CONTEXTS`、`FB_DEPTH`、`FB_ARGS`。
- [build-runtime.sh](../scripts/build-runtime.sh)：`GW_CONTEXTS` 的默认值和范围校验。

## 2. 长度边界与截断

以下长度按 C 缓冲区中的字节计，不按 Java 字符数计。

| 项目 | 当前有效上限 | 当前失败语义 |
| --- | --- | --- |
| 请求路径、解析后的映像路径、libjava 路径 | 4095 字节正文 + 终止符 | 相关入口通常明确拒绝超长路径 |
| JNI 导出符号名 | 255 字节正文 + 终止符 | 后端拒绝超长查找 |
| JNI 方法描述符 | 255 字节正文 + 终止符 | 反射、ID、注册、查找和命令路径均先检查再完整复制；超长拒绝，不以截断键继续 |
| Guest 附着线程名 | 63 字节正文 + 终止符 | 超长截断 |
| 错误和诊断消息 | 511 字节正文 + 终止符 | 超长截断，可能丢失诊断细节 |
| JVM profile 名称 | 127 字节正文 + 终止符 | 必须在固定数组内终止 |
| v2 profile 的 libjava / libjvm SHA256 | 64 个小写十六进制字符 + 终止符 | 长度或字符格式不符拒绝 |

反射得到超长方法/字段描述符时抛 `IllegalArgumentException`，释放临时 UTF 字符串和 local 引用，不插入截断 ID。命令超长时在改变槽位、深度和序号前拒绝。注册和显式查找维持各自的 JNI/结构化错误返回契约。

边界证据包括桌面 ASan/UBSan 的 255/256 字节反射和命令检查，以及 Zig 扩容夹具每个 ClassLoader 周期三项真实反射检查：255 字节方法描述符接受，超长方法和字段描述符拒绝。

源码位置：[loader_types.h](../bionic-dlopen/include/loader_types.h)、[loader_backend.c](../android-jni/src/host/loader_backend.c)、[references.c](../android-jni/src/host/references.c)、[bindings.c](../android-jni/src/host/bindings.c)、[command.c](../android-jni/src/host/command.c)、[jvm_adapter.h](../jvm-jni/include/jvm_adapter.h)。

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

分类器不是完整 ELF 验证器，不代替实际 GNU/bionic loader 的重定位与符号检查。这些上限目前仍保留，是否扩大或改用其他数据结构需另行审阅，不能仅改回模糊的布尔结果。

源码位置：[elf_route.h](../bionic-dlopen/src/elf_route.h)、[elf_route.c](../bionic-dlopen/src/elf_route.c)、[loader.c](../bionic-dlopen/src/loader.c) 的 `loader_open`。测试：[elf_route.py](../bionic-dlopen/tests/elf_route.py)、[loader_contract.c](../bionic-dlopen/tests/loader_contract.c)。

状态：49 项 ASan/UBSan 元数据检查、纯 C 装载检查、删除前 C 对照及默认 Zig Android 真机组合回归通过，见第 9 节。

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
| 启动参数保存数组 | 16 项 | 写入前要求 argc 为 1..16、每项指针非空；不接受 0 项 |
| 启动环境 | 当前只构造 2 项 | `LC_ALL=C`、关闭 rseq 的 `GLIBC_TUNABLES`；不自动继承完整 Host 环境 |
| 启动栈空间 | 调用方提供 | 写入前检查地址溢出及参数、环境、auxv、随机字节、对齐和 4 KiB 余量预算 |
| 启动失败 | 部分路径直接 `exit(1)` | 不能当作返回错误后可恢复重试的接口 |
| GNU 映射生命周期 | 保留至进程退出 | 尚无完整销毁映射并重新创建 runtime 的通用 API |

AUD-04 的保护页/共享哨兵测试覆盖 10 类非法输入及有效 1/16 项参数和非对齐栈顶。拒绝发生在首次栈写入前；这不改变 bootstrap 失败会终止进程的接口边界。

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
| 注册函数归属 | 有当前模块时归该逻辑模块，可使用其依赖函数；无上下文时逐个函数严格匹配同一顶层 `link_map`，共享依赖/未知/混合所有者整批拒绝 |
| 同类注册所有权 | 一个类一个模块；更新在途时互斥拒绝；显式注销成功释放记录，旧模块卸载不注销新所有者的注册；不确定失败隔离并保留资源 |
| 停止 | 有加载中、活跃或关闭中模块时拒绝停止；quarantine 后也拒绝停止/重配以保留资源 |
| 不确定清理失败 | UnregisterNatives、弱类提升 OOM、在途注册或 Guest 提交/清理失败时永久隔离桥，保留仍可能被引用的资源；宿主丢弃进程 |
| 同线程回调中关闭祖先模块 | native/查找/注册持有执行 scope；关闭当前线程仍持有的任意祖先返回 BUSY，不进入 CLOSING；可跨线程归还的 pin 单独记账 |
| 持有 scope 时关闭其他活跃模块 | 目标 active 非零即 BUSY，不进入借用等待；无 scope 的线程仍可等待，目标无活跃借用仍可关闭；调用方退出回调后再处理 BUSY，不自旋重试 |
| 后台线程 | 模组必须停止并 join；桥不会自动发现任意后台线程或缓存函数指针的借用 |
| 等待和取消 | 无通用 deadline / 超时取消协议；对端卡住可能无限等待 |
| 跨 libc 对象 | 不可自由混用 allocator、`FILE`、pthread 对象、C++ 对象及异常；Guest 内部使用 C++ 不因此被禁止 |
| 进程故障隔离 | 同进程执行；Guest 原生崩溃和进程级 `exit` 没有进程隔离 |
| Host/Guest 协议 | 配置必须匹配并成套部署；JNI 能力与装载协议 v5 均要求 Guest 确认；核对共享区大小、上下文数和模块上限后才 ready |
| runtime 组合 | 模块化 DSOs 与兼容单 DSO 二选一，不能在同一进程重复装入两套单例状态 |

存储初始容量集中在 [resource_limits.h](../android-jni/src/host/resource_limits.h)。超过初始容量时按块增长，已有地址和 token 不移动；空块保留并复用至 store 销毁。初始容量不能防止无限资源请求或保证扩容后的性能，ID/类身份路径仍扫描已分配容量。

正常卸载顺序为 OnUnload（仍映射）→ 注销 VM 入口 → dlclose → 释放跳板/清扫 ID。拒绝装载先注销入口，再 ABORT，确认成功后释放。任何不确定清理失败均返回 `GW_E_QUARANTINED`，不继续释放可能可达的资源、不复用 LOADING/CLOSING 槽、不自动重试；宿主可调用 `gw_dlopen_is_quarantined()` 检查终态。隔离不可撤销，不自动终止应用，也不取消已在执行的任意 Guest 代码。

注册发布期间持有模块借用；关闭后拒绝新注册。故障注入验证清理顺序、异常保留、注销/提交/ABORT/dlclose 失败和隔离后无新增命令。它没有证明任意 VM 线程与缓存入口的并发卸载安全；宿主仍须同步并停止后台线程。

源码与契约位置：[jni_capabilities.h](../android-jni/src/common/jni_capabilities.h)、[full_guest.c](../android-jni/src/guest/full_guest.c)、[guest_loader.c](../android-jni/src/guest/guest_loader.c)、[full_host.c](../android-jni/src/host/full_host.c)、[bindings.c](../android-jni/src/host/bindings.c)、[JvmAdapter.java](../jvm-jni/src/java/dev/glibc/worker/JvmAdapter.java)、[LOADER_BACKEND.md](../android-jni/docs/LOADER_BACKEND.md)。

### AUD-07 / AUD-08 交互与生命周期规则

AUD-07 管理线程身份、容量、所有权和借用；AUD-08 明确进程生命周期及等待失败的处置。通用强制取消不在支持范围；监督和丢弃执行进程属于宿主。

| 问题 | 当前实现与风险 | 审定规则与边界 |
| --- | --- | --- |
| 上下文生命周期 | Host 绑定至线程退出，Guest 附着至 detach；初始 16、按 16 项增长，退出后槽可复用，已发布页保留到进程退出 | 已完成；不改变线程 JNI/TLS 身份，不保证无限资源或自动发现后台工作 |
| 重入深度 | 初始 12、按 16 层增长，帧地址稳定；独立应急 RPC 及禁止应急重入保护；v5 握手验证布局 | 已完成；实际 VM 栈/原生栈与索引溢出边界仍存在；诊断 handoff 只记录前 12 层 |
| 执行 scope 与关闭等待 | 连续 TLS 表按模块代数记录当前线程持有的全部模块和递归次数；关闭祖先，或持有 scope 时关闭有活跃借用的其他模块，均在 CLOSING 前返回 BUSY；借用归还的正常等待保留 | 已登记 scope 与 loader 借用等待构成的环被保守拒绝；纯 pin 环、OnUnload/transport、用户锁/join/RPC 和未登记后台工作仍需宿主管理；完整规则见执行借用契约 |
| 类注册所有权 | 一个类一个模块；注册/注销预留排斥其他更新，成功显式注销释放所有权；有主类注销不确定失败隔离桥 | 已完成；交接后旧模块卸载不能注销新所有者的类；未假设任意 VM/缓存入口并发卸载安全 |
| 依赖库归属 | 当前调用/装载上下文决定逻辑所有者；无上下文须逐项得到同一顶层模块，拒绝共享依赖和混合批次 | 已完成；无上下文共享依赖注册明确不支持；Guest 自行管理的任意映像/函数指针寿命不自动推断 |
| 发布后的 RPC 超时 | 命令仍可能访问 caller 栈上的参数、Guest 地址或 JNI frame；隔离 registry 不会延长这些借用对象的寿命 | 宿主 deadline 仅触发监督；不提供通用取消确认。真实响应/可靠协作确认前保留栈、引用、buffer 与 scope；无法确认安全时隔离并处置进程，隔离本身不解除调用等待 |
| 关闭与退出 | active 借用、事件等待、pthread join 均可能无期限阻塞；不能强制结束任意 Guest 函数并保证资源安全 | 所有者先停止生产者、完成模组工作并关闭模块；保守拒绝在途请求/回调和线程交接，允许静止的 Host 绑定。shutdown 同步且无内置期限；启动后的失败或关闭中的不确定失败隔离，禁止同进程重启；不自动杀应用 |

旧版自等待由本地 [C 最小用例](../bionic-dlopen/build/aud07/ancestor_close.c) 和 [审阅结果](../bionic-dlopen/build/aud07/review.json) 记录：进入 CLOSING 后由外部 watchdog 结束独立进程。修复后的维护用例为 [execution_scopes.c](../bionic-dlopen/tests/execution_scopes.c)，真实 JNI 证据见 [本轮 coverage](../jvm-jni/build/aud07/coverage-final/summary.json)：四例、四次 BUSY、异常展开后继续调用、最终零引用/closure。新规则借鉴 LRPC 执行链记账和 SRCU 禁止自等待，未移植完整 SRCU；调用方契约和剩余边界见 [EXECUTION_SCOPES.md](../bionic-dlopen/docs/EXECUTION_SCOPES.md)。

跨线程对照由 [close_waits.c](../bionic-dlopen/tests/close_waits.c) 维护。修改前的 [基线复现](../bionic-dlopen/build/aud07-waits/baseline.json) 确认两个线程都持有 scope、两个目标均进入 CLOSING；归还被各自的 close 返回阻断。修复后使用 barrier 保持所有 scope，要求两/三线程 close 全部 BUSY，且 pin 等待、探测后状态变化和无活跃借用目标关闭符合契约。真实 JNI 两线程对照见 [coverage](../jvm-jni/build/aud07-waits/coverage-final/summary.json)。

生命周期的主机回归为 [lifecycle.py](../android-jni/tests/lifecycle.py)（单例/所有者/终态及故障注入）和 [audit.py](../android-jni/tests/audit.py)（在途栈保留、迟到响应、外部父进程处置）。真实 Android JRE 的 [coverage](../jvm-jni/build/aud08/coverage-final/summary.json) 与 [smoke](../jvm-jni/build/aud08/smoke-final/summary.json) 同时验证正常桥关闭、GNU 主线程退出和 VM 销毁；允许协调静止的 Host 绑定，不把空闲 JVM 线程误判为在途调用。报告检查器拒绝生命周期记录缺失、重复或任何失败字段。主机替身不替代 Android 验证，破坏性停滞不计作真机取消能力。

## 7. 运行时制品、部署和适配

| 项目 | 当前边界 |
| --- | --- |
| glibc 制品 | 固定构建经过补丁的 glibc 2.44，loader/libc 必须配对部署 |
| 补丁应用验证 | 构建脚本隔离父级 Git 环境，核对反向补丁；跳过补丁或损坏缓存不能当作成功，见 [RUNTIME_PATCHES](../core/docs/RUNTIME_PATCHES.md) |
| clone3 | 当前 glibc 补丁改走旧 clone 路径 |
| rseq | 当前补丁和启动环境关闭登记 |
| robust-list | 当前补丁关闭登记；robust mutex 初始化返回 ENOTSUP，普通 non-robust 锁保留 glibc 实现，见 [mutex 契约](../core/docs/MUTEXES.md) |
| 依赖闭包 | 不自动下载或补齐完整 rootfs；必须部署模组需要的 GNU 依赖 |
| 默认兼容 stage | 主要部署 loader/libc/libpthread/libm；不能将其等同于包含所有第三方库的 rootfs |
| 映像策略 | 默认按安装目录或内容散列目录解析；目录外请求需匹配同字节已安装映像，或显式开启直接路径模式 |
| 目录映射条目的名字 | `lib[A-Za-z0-9_.-]+\.so`，并检查规范化后的父目录和内容一致性 |
| Android 执行权限 | 直接路径模式不绕过 Android 的路径、可执行映射及系统调用策略 |
| JVM 私有接口 | 选择审阅后的接口族并匹配 libjava/libjvm 两个 SHA256；字段、方法或原生符号不符时拒绝 |
| profile 生命周期 | 只在未安装、无并发装卸载和调用时配置；已安装时不能更换 |
| 安装失败恢复 | 尝试恢复原钩子；再次失败时保留状态并要求丢弃进程 |
| Android 构建基线 | 当前组合构建 API29，测试应用 minSdk29 / targetSdk36；这不是所有更低 Android 版本已被证明不能支持的结论 |
| 特定 JRE 的启动 | 测试宿主包含固定制品的私有启动修补，不能当作所有 Android JRE 的通用启动接口 |
| ART | 公开 JNI 层与 JRE 私有层已分离，但完整 ART 桥未验证 |
| fork、任意信号处理与完整销毁重建 | 没有完整兼容性验证，不因普通 JNI 测试通过而自动成立 |

AUD-06 已在旧版 ARM64 同进程 Android Guest 中复现：12 种 robust 配置均创建成功，但持锁 pthread 正常返回后 timedlock 返回 ETIMEDOUT，未获得 EOWNERDEAD。新补丁在对象写入前返回 ENOTSUP；主机标准 glibc 正对照覆盖 EOWNERDEAD、consistent 后复用和 ENOTRECOVERABLE。新 runtime 的 QEMU 与真机组合各通过 125 项检查，含普通锁、递归、errorcheck、跨线程超时和实际条件变量等待/唤醒。它不证明完整 pthread/PI/跨进程兼容。

AUD-10 关闭的是制品识别和验证证据边界，不是所有 Android JRE 的启动适配。清单固定归档、ABI、libjava/libjvm 配对和接口族；生产安装只使用已驻留 libjava，核对原方法归属、目标 JavaVM 实例与实际 libjvm 散列，v1 profile 明确拒绝。固定 Android ARM64 JRE17/21/25 的 smoke/coverage 在 CheckJNI 下完整通过；JRE8 严格 smoke、正常 JIT coverage 通过，但严格 coverage 在 JNI_COMMIT 后发生 VM 缓冲区释放崩溃。无桥 JNI 最小用例对照证明该问题独立于 glibc-worker，正常模式通过、CheckJNI 模式退出 134；严格组失败仍保留，不以正常组替代。测试宿主复用固定 Pojav JLI 启动路径、原始目录布局及 linker64 代理探针；它们不进入生产 SDK。桥的 native 返回清理前补充 ExceptionCheck，保留待抛异常。桌面五套 VM 各通过 31 项正常断言和 3 项恢复失败断言，不能替代 Android 双 libc 结果。具体模式边界与重现方式见 [JRE 契约](../jvm-jni/docs/JRE_COMPATIBILITY.md) 和 [测试入口](../jvm-jni/tests/README.md)。

AUD-11 已按不保留名称兼容的决定删除旧 C 头/符号、Java 包装类、构建宏 fallback 及旧 Host 枚举。SDK 安装到全新暂存目录后替换生成前缀，Java classes 编译前清理，避免增量构建夹带已删除接口。迁移入口为 [jvm_adapter.h](../jvm-jni/include/jvm_adapter.h)、`JvmAdapter` 和 `GW_HOST_VM_ADAPTER`；旧客户端需重新编译。SDK 检查同时验证旧头、符号、类和枚举不再导出。

源码位置：[build-glibc.sh](../core/scripts/build-glibc.sh)、[Termux NPTL 补丁](../core/patches/termux/set-nptl-syscalls.patch)、[clone3 补丁](../core/patches/termux/disable-clone3-glibc244.patch)、[package-runtime.sh](../android-jni/scripts/package-runtime.sh)、[GuestImages.java](../android-jni/src/java/dev/glibc/worker/GuestImages.java)、[profiles.c](../jvm-jni/src/native/profiles.c)、[ADAPTER.md](../jvm-jni/docs/ADAPTER.md)。

## 8. 等待、诊断和长期计数

| 项目 | 当前值或风险 |
| --- | --- |
| 主动等待预算 | 默认 20 μs，组合脚本允许 0..1 ms；不是 RPC 延迟上限，空闲路径使用 futex |
| RPC profiling | 默认关闭；开启后约 1/64 采样、64 个直方图桶，保留 16 条最差交接记录 |
| 通知过滤 / ready polling | 当前默认均关闭；开启时 Host、Guest 必须使用匹配配置 |
| 模块代数 | 32 位 unsigned；每槽达到 `UINT_MAX` 后不再复用，避免回绕 |
| 引用 / ID 代数 | 32 位，达到 `UINT32_MAX` 后永久退役槽位，C/Zig 均不回绕；token 仅在当前 store 生命周期内有效 |
| RPC 请求序号 | 有符号 32 位等值令牌，显式 INT_MAX→INT_MIN；-1→0 有效，每槽最多一个在途请求 |
| 统计快照 | 实时原子采样，不是事务视图；采样尾部和计数器不能当作精确的全量性能数据 |

引用槽位通过缩小配置中的代数上限验证耗尽退役，不以运行 2³² 次替代。RPC 在四种通知/轮询组合下以真实 pthread/futex 交换跨过 INT_MAX 和零；序号不得排序或作为并发生产者分配器。

源码位置：[loader.c](../bionic-dlopen/src/loader.c) 的 `reserve`、[references.c](../android-jni/src/host/references.c) 的引用分配路径及 [slot_store.zig](../android-jni/src/host/slot_store.zig)、[command.c](../android-jni/src/host/command.c) 的 `fh_command`、[rpc.c](../android-jni/src/guest/rpc.c) 的 `fg_rpc`、[rpc_metrics.h](../core/include/rpc_metrics.h)、[rpc_handoff.h](../core/include/rpc_handoff.h)、[build-runtime.sh](../scripts/build-runtime.sh)。

## 9. 已有证据与未完成验证

默认 ARM64 组合配置为初始 16 contexts、profiling/通知过滤/ready polling 关闭、主动等待 20 μs；运行时允许扩容。AUD-07 另验证 profiling/过滤/polling 同时开启的组合。两份独立 APK 和 run_id 证据见下，不将可变构建目录或外部 Pojav 历史包当作本轮制品。

| 验证 | 已有证据 | 尚不能证明 |
| --- | --- | --- |
| 桌面 JRE8/11/17/21/25 适配器 | 每个 VM 31 项正常契约断言及 3 项恢复失败断言通过 | 所有 Android JRE 制品、双 libc、所有新增 JNI 能力均兼容 |
| Android 固定 ARM64 JRE8 / 17 / 21 / 25 | 17/21/25 严格覆盖通过；8 正常 JIT 完整覆盖及严格 smoke 通过：142 项 Java/JNI、26 项装载、234/235 槽功能触达；Guest 退出与 VM 销毁完成。JRE8 GetModule 验证明确拒绝 | JRE8 的 CheckJNI JNI_COMMIT 路径（VM 独立缺陷）、完整 JNI 规范、所有源码分支或其他制品；历史 C 对照仍只针对固定 JRE17 |
| ELF 分类三态与边界 | 49 项 ASan/UBSan 检查、C 装载检查和当前 Android 组合回归通过 | 真实 GNU loader 的完整 ELF 验证 |
| Zig Host 资源管理 | 桌面 8,192 个存储 binding、32,768 个引用、1,024 个注册类；Android 三轮各 1,024 个方法、20,000 个引用和 320 个注册类通过，最终 closure/引用为 0 | 所有资源上限已取消、任意并发卸载安全或性能提升 |
| 审计边界与失败隔离 | core 保护页/哨兵和四种 RPC 配置、13 项生产 transport 故障场景、C/Zig 注册失败资源保留、C loader 三种隔离场景及 runner 结果生命周期检查通过 | 任意并发卸载安全、无限等待的取消 |
| AUD-03 失败语义 | 生产 RPC/引用/ID/上下文/帧、私有记录/TLS/worker 故障注入和 libffi/注册回滚检查通过；动态扩容后真机不再把 12 层/16 上下文当作预期错误，要求三轮零桥错误及最终零引用/closure | 全系统 OOM 行为、任意第三方模组的异常恢复、ART 完整兼容 |
| AUD-07 交互规则 | scope 的 CTest 7/7；共享页 48 线程/96 层/五处映射失败恢复；Zig owner 扩容；类更新互斥、交接、歧义归属和注销失败隔离；默认与诊断真机均通过 Host/Guest 各 32 线程、容量 16→80、三轮 32 层重入、142 Java / 26 装载及最终零资源 | 通用死锁检测、用户代码等待、任意后台入口追踪或安全取消；这些不是 AUD-07 完成的结论 |
| runtime 补丁流程 | 8 项回归检查；新 robust 拒绝补丁已核对源码、匹配 loader/libc 与 APK 内散列；ARM64 coverage 通过 | robust mutex 恢复能力（本 runtime 明确拒绝）、所有 pthread 属性 |

本地结果在各项目 `build/`，不是源代码发布包的必备文件：

- [桌面适配器结果](../jvm-jni/build/adapter-tests/summary.json)。
- [ELF 边界结果](../bionic-dlopen/build/test-results/aud01-elf-route.json)。
- [删除前 C 对照](../jvm-jni/build/aud01/coverage-c-control-v3/summary.json) 与 [AUD-01 默认 Zig 真机结果](../jvm-jni/build/aud01/coverage-default-clean/summary.json)。
- [AUD-01 容量与对照汇总](../android-jni/build/aud01/summary.json) 保存当前容量边界、删除前对照及删除后默认回归证据。
- [AUD-06 验证汇总](../core/build/aud06/summary.json)、[旧版真机失败复现](../jvm-jni/build/aud06/baseline-smoke/report.txt) 与 [新版真机覆盖](../jvm-jni/build/aud06/coverage-patched/summary.json) 区分原始缺陷、显式拒绝及普通锁/JNI 回归。
- [AUD-03 验证汇总](../android-jni/build/aud03/summary.json) 及 [真机覆盖](../jvm-jni/build/aud03/coverage-final/summary.json) 记录故障注入、应急 RPC、资源恢复及本轮制品。
- [审计验证汇总](../android-jni/build/audit-fixes/summary.json) 汇合边界、故障注入、真机和 SDK 检查；[Host 资源管理验证](../android-jni/build/zig-resources/summary.json) 保留先前扩容证据。

旧 runner 产生的历史目录可能保留过期 PASS，不能据此推定当前尝试通过。当前 runner 在任何设备检查前隔离旧制品，发布的 summary 必须与 run-state.json 同为 PASS、run_id 一致，并且 apk_sha256 与 installed_apk_sha256 一致。失败记录为 FAILED/INTERRUPTED；强制终止留下的 RUNNING 不构成通过。目录中存在旧 PASS 文件本身不是证据。

| 记录 | SHA256 |
| --- | --- |
| 删除前 C 对照 PASS APK | `98467869d7a1061888fe2831319f3fd7fe5d2da2708122fe19064daf8394e217` |
| AUD-01 默认 Zig PASS APK | `93e4e50a381fbea8fabcccd1f2420247506bf92ad7be7ba1737ea7a73e6dcf3b` |

AUD-09 的验证包括真实 runner 的设备发现失败：旧 PASS 在 ABI 查询前已归档，本轮没有成功 summary。结果目录互斥不代替设备级互斥，同一手机仍必须按顺序运行。

原始兼容性和复现命令继续维护在各项目契约及 [测试手册](../jvm-jni/tests/README.md) 中。修复本清单条目时，应同步更新对应状态、限制值和验证范围，不将源码检查或单项测试自动升级为完整兼容性声明。

AUD-07 当前可复现证据：

- [共享页并发/OOM 结果](../android-jni/build/context-page-tests/summary.json)。
- [默认组合 coverage](../jvm-jni/build/aud07-final/coverage-final/summary.json) 与 [诊断/过滤/polling 组合](../jvm-jni/build/aud07-final/coverage-profile-final/summary.json)。
- [本轮汇总](../android-jni/build/aud07-final/summary.json)，包含具体制品散列、run_id、桌面替身与真机验证的区别。
