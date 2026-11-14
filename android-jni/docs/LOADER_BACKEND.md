# JNI 装载接口与宿主接入

本项目把 JNI 生命周期和调用约束适配到 bionic-dlopen 的 C 装载层。GNU 库始终由完整 Guest loader/libc 线程域加载；不会在 bionic 线程上直接调用 GNU 函数指针。

## 依赖和接口分工

| 组件 | 职责 |
| --- | --- |
| `include/loader_backend.h` | v1 JNI 装载函数表、显式方法描述符和结构化错误 |
| `host/loader_backend.c` | JNI 版本/描述符检查，转换 C transport 与 JNI 结果 |
| `host/loader_transport.c`、`host/command.c` | 装卸载事务相位、异常保留和 RPC 命令提交 |
| `include/host_bridge.h` | Host 配置与公共桥接生命周期 |
| `include/jni_vm_adapter.h` | 可选 VM 扩展的注册、安装与恢复 |
| `host/image_policy.c`、`GuestImages` | 按原始内容匹配已安装映像或显式使用直接路径 |
| `guest/` | GNU JNI 代理、Guest 装卸载和 RPC 服务 |

句柄代数、重复 inode、模块状态、活跃借用和关闭等待由 [bionic-dlopen](../../bionic-dlopen/README.md) 管理。固定 JRE 的 NativeLibraries 钩子和反射签名处理属于 [jvm-jni](../../jvm-jni/docs/ADAPTER.md)。

## 查询与调用

```c
const struct gw_loader_api_v1 *api =
    gw_loader_query(GW_LOADER_ABI_V1, sizeof(struct gw_loader_api_v1));
```

查询可能返回 NULL；调用者必须检查 ABI 和大小。JNIEnv 必须来自当前附着线程及目标真实 VM；不得借用其他线程或其他 VM 的 JNIEnv。

| 操作 | 约定 |
| --- | --- |
| `open(env, path, flags, &library, &error)` | AUTO 根据 ELF GNU 标记决定路由；GUEST 由调用方显式确认 GNU ABI。成功返回逻辑句柄及 JNI 版本 |
| `find(env, handle, symbol, descriptor, &entry, &error)` | 完整描述符如 `(J)J`；返回 bionic Host 跳板 |
| `close(env, handle, &error)` | 阻止新借用，等待已有调用，完成 Guest OnUnload/dlclose 与注册清理 |
| `owns_handle(handle)` | 已退休的 Guest tag 也返回真，防止误传 bionic loader |
| `module_state(handle)` | loading=1、live=2、closing=3、stale=-1，仅供诊断 |

`GW_PASS` 允许继续原平台 loader；`GW_OK` 表示 Guest 已处理；`GW_NOT_FOUND` 仅表示符号未找到；`GW_ERROR` 终止该次 Guest 操作，不得再把同一 GNU 文件交给 bionic 重试。保留已有 JNI pending exception，避免覆盖原 Java 异常。

AUTO 分类损坏或不支持的 ELF 返回 IMAGE，分类器资源上限返回 CAPACITY，均属于 ERROR。只有成功解析且未发现 GNU 标记才允许 PASS，不能把“无法判断”解释为“原平台可加载”。

路径、符号和描述符只在调用期间借用。描述符限制为 32 个参数/256 字节；完整性检查不证明它与任意 ELF 函数原型一致，宿主必须传递真实方法 metadata。函数指针只在库保持打开时有效。宿主负责同步方法调用与卸载，不得在 close 后继续使用缓存跳板。

## 启动和停止顺序

1. 准备目标真实 VM、可见的 `BridgeTypes`/`GuestImages` 辅助类及共享协议区。
2. 若需要 VM 私有装载钩子，先注册 `gw_jni_vm_adapter_v1`，再在配置中请求 `GW_HOST_VM_ADAPTER`。
3. 在目标 VM 的有效 JNI 线程调用 `full_host_start`，显式传入目录、映像策略和 flags。
4. 使用匹配的 GNU loader/libc 启动 Guest 线程域，等待 `full_guest_start` 和 RPC 服务可用，再调用 `full_host_ready`。
5. 开放模组加载。ready 前的 GNU 操作返回 NOT_READY；普通 bionic 操作仍可按原平台规则处理。
6. 关闭所有 Guest 模块后调用 `full_host_shutdown`，停止后端、恢复可选适配器、释放 Host helper 与引用缓存；宿主随后确认 raw thread 退出，Guest pthread 由 Guest 自己 join。

加载中、活动中或关闭中的模块会阻止停止。native 跳板、查找和注册发布持有当前 Host 线程的执行 scope；同步重入时记录全部祖先模块及递归次数，正常返回和异常展开都配对归还。从回调关闭当前线程仍持有的任意祖先模块返回 BUSY，且不进入 CLOSING。当前最内层 module 字段只表示调用归属，不替代执行借用记录。

持有任意执行 scope 时，关闭还有活跃借用的其他模块同样返回 BUSY，防止两个回调各持一个模块并等待对方。宿主应退出回调、归还 scope 后再关闭，不得在 scope 内自旋重试。没有 scope 的生命周期线程仍可等待其他线程归还借用；目标没有活跃借用时允许关闭。

这不是通用死锁检测；纯 pin 环、OnUnload/transport 和用户代码内的其他等待尚无通用处置。借用计数不代表任意原生后台线程，模组必须自行停止并 join 后台工作。两类借用及限制见 [执行借用规则](../../bionic-dlopen/docs/EXECUTION_SCOPES.md)。

当前进程只支持一次配置、一套 Guest 和一个目标 VM。初始化失败后的完整重试、GNU loader/libc 映射销毁与重建尚无通用 API；失败无法安全恢复时应结束隔离的宿主进程。shutdown 不替宿主调用 DestroyJavaVM。单实例状态、所有者线程、关闭前停止生产者，以及 deadline/取消/进程处置规则见 [生命周期契约](LIFECYCLE.md)。

## 卸载与失败隔离

正常 close 依次执行：Guest `JNI_OnUnload`（映像保持映射）、Host 注销该模块的 VM native 入口、Guest `dlclose`，最后释放跳板并清扫 ID。拒绝的 OnLoad 先撤回注册，再执行不含 OnUnload 的 ABORT，确认成功后才释放跳板。清理保留最初的 Java 异常，不用后续异常覆盖它。

弱类提升发生 OOM、注册预留仍在途、`UnregisterNatives` 失败或 Guest 提交/清理命令失败时，桥进入永久 quarantine。尚可能被使用的注册记录、closure、映像和上下文保留；已成功完成的清理不回滚。返回 `GW_E_QUARANTINED`，宿主必须丢弃进程。桥不自动杀应用，也不允许在同进程恢复。注册在发布 VM 入口期间持有模块借用，CLOSING 或 quarantine 后拒绝新注册；已保留的跳板拒绝新的 Guest 调用。

这不替代宿主对缓存入口和原生后台线程的同步，不能据此承诺任意并发卸载安全。桌面故障注入 `python3 android-jni/tests/audit.py` 使用生产 transport/command 和 VM/Guest 替身检查顺序、保留和异常语义；它与真机正常生命周期回归互补。

线程绑定、动态容量、成功注销后的类交接以及无上下文依赖函数的拒绝规则见 [上下文与所有权](CONTEXTS_AND_OWNERSHIP.md)。

## JNI 能力协商

桥实现 JNI1.6 基础表及 GetModule，最高声明 JNI10。Host 启动时取真实 VM 的 GetVersion，与桥上限协商；在空闲启动槽 10..12 中交换能力并等待 Guest 确认。未确认的 Guest 不能 ready，Host 与 Guest 必须成套部署，即使关闭诊断和通知过滤也不能跳过此握手。

私有装载协议版本为 5：在同一空闲启动槽的 13..23 项确认协议、共享结构与调用帧大小、初始上下文/帧容量、页宽及固定模块上限 64，确认后才激活 loader。v5 使用动态目录和独立应急 RPC，不能混装 v4 或布局/配置不匹配的 Host/Guest。

Guest GetVersion 返回协商值，GetEnv 和 AttachCurrentThread 拒绝更高或未知的版本并清空输出指针。JNI_OnLoad 的返回值也在提交装载前按同一能力检查，失败走正常回滚。JRE8 宿主最高 JNI1.8，较新 Host 最高 JNI10；这不限制宿主 Java 字节码版本，但不提供 JNI19 及以后新增槽位。

低于 JNI9 的宿主收到 GetModule 请求时抛 UnsupportedOperationException；不会读取宿主 JNI 表的第 233 槽。完整 ART 行为仍需独立验证。

方法/字段描述符最多 255 字节正文。反射、ID 缓存、注册和命令入口检查长度后完整复制，拒绝超长类型信息；反射和命令路径抛 `IllegalArgumentException`。上下文和调用帧动态增长，扩容失败保留原异常或抛 `OutOfMemoryError`；实际 VM 的栈溢出异常继续保留。返回码、回滚和应急槽的完整契约见 [JNI 失败处理](FAILURES.md)。

## 配置和兼容边界

v2 配置保留历史 `jre_library_path` 字段，仅交给可选适配器。`GW_HOST_LEGACY_JRE17` 保留为 `GW_HOST_VM_ADAPTER` 同位别名；请求适配器却未注册会在初始化早期失败。`GW_HOST_DIRECT_IMAGES` 允许直接映像；默认内容匹配要求宿主提前部署相同原始二进制。路径可执行性仍受 Android 策略限制。

大数组策略见 [副本缓存](ARRAY_COPY_CACHE.md)。Host/Guest 只能共享约定的 POD、地址与长度，allocator、FILE、pthread、异常对象及 JNI 引用的所有权不能跨 libc 自由互换。

已有真实测试使用 Android HotSpot。实现使用公开 JNI 并不构成 ART 完整兼容证明；HotSpot 宿主不能把 Android 应用入口的 ART JNIEnv 传入此桥。本接口也不全局劫持任意 bionic dlopen；GNU 库内部 dlopen 则继续使用 Guest loader。
