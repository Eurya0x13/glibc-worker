# 上下文、重入与注册所有权

本契约规定 JNI 交互的线程身份、共享调用帧和 native 注册生命周期。装载层的执行借用另见 [执行 scope](../../bionic-dlopen/docs/EXECUTION_SCOPES.md)，不可恢复失败见 [失败处理](FAILURES.md)。

## 线程绑定和容量

Host 线程首次进入桥时绑定独立上下文及 Guest worker，保持到线程退出；Guest 自建线程 AttachCurrentThread 时绑定独立上下文及 Host JNI helper，DetachCurrentThread 后由控制线程回收。上下文保持线程的 JNI/TLS 身份，不会在每次调用结束后移交给无关线程。Host 来源线程不能通过 Guest DetachCurrentThread 分离。

`GW_CONTEXTS` 是初始内联容量，默认 16，构建时可设 2..256；包含 owner 上下文。Host 调用线程和 Guest 附着线程共享同一动态目录。没有空闲项时按 16 项增长，双方以 CAS 竞争单个 FREE 槽，不会等待其他线程归还旧槽来满足新的附着。退出、detach 或启动失败安全完成后，槽可复用；容量保留。

普通调用帧初始为每上下文 12 层，超过后按 16 层增长。Host 命令与 Guest JNI RPC 使用同一套稳定帧地址，已有 request/response、参数和返回值不移动。正常深度不再受 12 限制；实际可用深度仍受 VM/线程栈、内存及有符号索引溢出检查约束，不承诺无限递归。

增长失败时，Host 入口保留原异常或抛 OutOfMemoryError；Guest AttachCurrentThread 清空输出 JNIEnv 并返回 JNI_ENOMEM。Guest 另有一个独立应急 RPC，不需要分配新的调用帧即可报告失败、查询/清除异常或执行必要释放；它不绕过 JNI 线程身份规则。整数边界按资源不足拒绝，不能发生有符号溢出后继续访问。

## 存储与发布

共享目录采用四级、每级 256 分支的 radix 索引，叶页保存连续 16 项。初始项内联，扩展页由当前 libc 域调用 mmap 建立；目录和叶页只通过原子 release/acquire 发布，竞争失败者只 munmap 自己尚未发布的候选页。发布后的页不移动、不回收，保留到承载进程退出，避免另一域仍持有地址时释放。

共享页只有 POD、原子序号和稳定地址。两侧的 pthread、JNIEnv、allocator 和私有线程记录仍由各自 libc 管理；另一侧不能解引用私有记录。引用存储的每上下文索引使用 Zig 动态向量，索引增长不会移动已发布引用槽。

初始容量并非资源预算：每个绑定上下文还需要线程栈、VM 附着和私有状态。增长可能受操作系统和 VM 限制而失败。上下文复用不重置事件 epoch，也不丢弃已经发布的帧目录；关闭和永久隔离均不意味着可安全撤销任意在途工作。

协议升级为 v5。启动握手检查共享区、上下文/RPC/call 的布局大小、初始上下文和帧容量、叶页宽度及模块上限。Host/Guest 必须成套重编部署，不能混用 v4。诊断 handoff 明细只采样前 12 层，较深调用继续执行并计入其他统计；该诊断窗口不是执行深度上限。

## 类注册与交接

一个 Java 类的桥接 native 注册同一时间只归一个逻辑模块；类记录使用 weak 引用，不延长 ClassLoader 寿命。

1. RegisterNatives 在发布前预留类、持有模块执行 scope。一个类有注册/注销在途时，其他更新明确拒绝，不能覆盖或接管。
2. 到达真实 VM 前的准备失败撤销本次未发布记录和新建跳板。VM RegisterNatives 即使失败也可能发布部分方法，因此该阶段失败后继续保留所有权及跳板，直到安全注销/卸载。
3. 显式 UnregisterNatives 对有主类要求当前模块相同；无当前模块时可根据已记录类所有者借用模块。无主类也预留注销事务，防止注销与新注册竞态。
4. VM 注销成功且没有 pending exception 后，删除类所有权记录，其他模块可重新注册。只释放类记录，旧模块跳板仍保留到模块卸载，避免影响其他类或在途调用。
5. 旧模块卸载只注销仍归它所有的类，不得注销已经交接给新模块的类。新模块退出时负责自己的注册。
6. 对有主类的注销结果不确定时永久 quarantine，保留类记录、跳板和映像，由宿主丢弃进程；不自动杀应用或尝试恢复。

类操作的预留在锁内完成，RegisterNatives/显式 UnregisterNatives 的真实 VM 调用在锁外执行。调用方仍须同步 Java 方法调用与卸载；成功注销并不证明任意线程持有的缓存入口已经失效。

这些约束围绕 [JNI RegisterNatives / UnregisterNatives](https://docs.oracle.com/en/java/javase/26/docs/specs/jni/functions.html#registering-native-methods) 的类级注册与重新链接语义实现，不扩展为任意方法级多所有者模型。

## 依赖函数的归属

当前装载或 native 调用上下文存在时，注册归当前逻辑模块，函数可以来自其 DT_NEEDED 依赖。函数所在映像与生命周期所有者是不同概念：共享依赖地址不能用于猜测唯一模组所有者。

没有当前模块的 Guest 附着线程只能使用严格逆向解析：**每个**函数地址都必须解析为同一个已管理顶层模块的 link_map，再借用该模块发布注册。任意项来自未知映像、共享依赖，或多个顶层模块时，整批在发布 VM 入口前拒绝。不能只看第一个地址，也不按加载先后选一个所有者。

因此无上下文的共享依赖注册仍是明确不支持的情形；调用方须在已知模块调用路径中完成这类注册。桥不会自动证明任意函数指针与依赖的完整生命周期，Guest 内部自行 dlopen/dlclose 的对象仍由模组管理。

## 验证入口

- `python3 android-jni/tests/context_pages.py`：ASan/UBSan 的并发领取、稳定地址、96 层帧、页分配失败与恢复。
- `tests/failures.py`：生产 Host/Guest RPC 的分配失败、应急异常处理、私有记录/TLS/worker 失败及恢复，VM/worker 使用替身。
- `tests/bindings.py` / `tests/resources.py`：真实 libffi、类更新重入拒绝、交接、注销失败隔离，以及 Zig owner 目录增长和 OOM。
- `jvm-jni/tests/coverage/run.sh --device SERIAL`：真实 Android JRE 下三轮 Host/Guest 各 32 线程、32 层重入、共享依赖注册、后台顶层逆向归属、歧义/混合批拒绝与卸载后的所有权检查。既有 scope、mutex、JNI、回滚和最终零引用/closure 检查继续执行。

这些验证不等于完整 ART/JNI 兼容，不涵盖任意后台代码、用户锁、纯 pin 环、deadline 或安全取消；后者继续属于 AUD-08。
