# 执行借用与关闭规则

本契约解决同步重入中的自等待：`A → Java → B → Java → close(A)`。只检查当前最内层模块 B 会遗漏仍在执行的 A；关闭 A 若进入 CLOSING 并等待 active 清零，当前线程就无法返回并归还 A。

同一规则还覆盖跨线程的借用等待环：线程一持 A 关闭 B、线程二持 B 关闭 A。桥不构造完整等待图，而是在持有执行 scope 时拒绝需要等待其他借用的关闭。

## 两类借用

| 接口 | 含义 | 归还线程 | 本线程关闭 |
| --- | --- | --- | --- |
| `gw_dlopen_scope_enter/leave` | 当前 Host 线程正在执行模块或服务其同步回调 | 必须原线程 | 持有目标模块，或持有任意 scope 且目标有活跃借用时 BUSY |
| `gw_dlopen_enter/leave` | 保持映像存活，允许工作移交 | 可以其他线程 | 按正常关闭等待；不自动判断自等待 |

每次成功进入都增加模块 active；两类归还只能减少各自的借用。scope 的递归进入须逐次归还，不能以一次 leave 清空整个调用链。scope 不支持线程转移、强制取消或线程退出时自动释放；调用方必须在所有正常和错误出口配对归还。

## 状态规则

1. 仅 READY 后端的 LOADING/LIVE 模块接受新借用。隔离或计数耗尽时拒绝，不修改计数。
2. close 在锁内检查当前线程是否持有目标模块；若持有，返回 `GW_ERROR/GW_E_BUSY`，模块状态和资源不变。
3. 若目标还有活跃 scope/pin，且当前线程持有任意执行 scope，close 同样返回 BUSY，不改变目标状态。目标无活跃借用时允许关闭；当前线程没有执行 scope 时仍可正常等待其他线程归还借用。
4. transport 的 current_module 探测后再次检查 scope 与目标 active，随后才允许 LIVE → CLOSING。保留 current_module 检查以兼容未使用新 scope 接口的旧 transport，但它不能代替祖先借用记录。
5. CLOSING 拒绝新借用，其他线程仍可归还旧借用；最后一次归还唤醒 closer。
6. quarantine 唤醒等待者并禁止 teardown，已有借用仍可归还。它不表示 Guest 已停止执行。

BUSY 是本次关闭未执行的结果，不会自动排队、卸载或安排异步关闭。调用方应先退出回调并归还执行 scope，再由没有 scope 的生命周期线程按宿主同步规则关闭；不得保持 scope 不变并自旋重试。即使没有真实等待环，持有 scope 时关闭一个暂时被其他线程使用的无关模块也会被保守拒绝。

## 存储与上层接入

模块容量固定为 64，因此每个 Host 线程使用连续的 64 项 TLS 数组，按模块槽保存完整代数句柄和递归计数。它记录“持有哪些模块及次数”，不保存调用先后顺序，不是完整调用栈；不分配堆内存，不新增独立调用深度限制。旧代数句柄不能匹配重用后的槽位。

只有 close 在目标 active 非零时扫描至多 64 项本线程 scope；scope enter/leave 和每次 RPC 没有新增扫描或等待图维护。

装载层在 transport.find 期间自动持有 scope。JNI 层在 native 跳板执行及注册发布期间持有 scope，涵盖同步 Java 回调和嵌套 Guest 调用；包括 JNI pending exception 在内的返回路径都先归还 scope。当前最内层 module 字段仍用于 RPC/注册归属，不能用它代表整条调用链的借用。

新增 C 符号要求使用包含这些符号的 bionic-dlopen 制品；本次 scope API 不改变 v1 查询函数表布局。JNI 上层另须成套部署匹配的共享协议。不能只更新头文件并继续链接旧库。

## 仍需调用方保证

对于已登记的执行 scope，线程在保持任意 scope 时不会进入 loader 的 active 借用等待。因此两线程或多线程由这些 scope 与 loader.close 借用等待构成的环会被拒绝；不需要猜测某个 pin 属于哪个线程。

这不证明整个程序无死锁。可转移 pin 没有线程所有者，纯 pin 环仍需调用方避免。transport.close / JNI_OnUnload、用户 mutex、join、任意 RPC 中的等待不属于本次检查；目标 active 为零也不代表其 teardown 不会阻塞。Guest 自建后台线程、缓存函数指针和脱离同步回调链的工作也不会自动登记。

上下文扩容、注册类所有权与共享依赖归属由上层 [JNI 交互契约](../../android-jni/docs/CONTEXTS_AND_OWNERSHIP.md) 处理；本层仍没有 deadline/取消确认机制。宿主仍需同步卸载、停止并 join 后台工作；不能用超时返回释放仍可能被远端访问的栈或 JNI frame。任意等待的 deadline/取消仍属于 AUD-08。

## 研究依据与区别

[LRPC（1990）](https://graphics.stanford.edu/~tpurcell/quals/os/bers90.pdf) 的线程 linkage stack 提供了显式记录嵌套跨域调用的思路；这里仅借鉴执行链记账，没有移植其内核调度和域终止机制。

[Linux SRCU 规则](https://docs.kernel.org/core-api/kernel-api.html#c.srcu_read_lock) 要求避免直接或间接等待自身仍持有的读侧临界区。这里采用相同的“不等待自己的借用”约束，但继续使用模块锁、计数和条件变量，不是 SRCU 算法，也没有 SRCU 的完整 grace-period 实现。

## 验证入口

- 本项目 CTest：`execution_scopes`、`close_waits` 和 `quarantine_0..3`；生产 loader 配合 C transport，适合 ASan/UBSan，不声称建立双 libc。close_waits 通过 barrier 保证两/三线程都持有 scope，且所有 close 返回前不归还；检查拒绝后的 LIVE 状态、pin 等待、探测后的二次校验与无活跃借用目标关闭。
- `jvm-jni/tests/coverage/run.sh --device SERIAL`：真实 Android JRE/双 libc 下检查自身关闭、A→B 关闭 A/B、A→B→A 关闭 B 及异常展开；结构化 `AUDIT_SCOPE_TEST` 要求四例通过。既有装卸载、并发调用和最终零引用/closure 断言继续运行。
- `AUDIT_CLOSE_WAIT_TEST` 另行要求两个真实 JNI 调用同时回调 Java、互关对方模块均 BUSY，返回后可继续调用，未提前释放资源。测试 barrier/join 的期限只用于报告测试失败，不是生产 bridge 的超时或取消保证。
