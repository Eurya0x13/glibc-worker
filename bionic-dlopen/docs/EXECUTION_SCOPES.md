# 执行借用与关闭规则

本契约解决同步重入中的自等待：`A → Java → B → Java → close(A)`。只检查当前最内层模块 B 会遗漏仍在执行的 A；关闭 A 若进入 CLOSING 并等待 active 清零，当前线程就无法返回并归还 A。

## 两类借用

| 接口 | 含义 | 归还线程 | 本线程关闭 |
| --- | --- | --- | --- |
| `gw_dlopen_scope_enter/leave` | 当前 Host 线程正在执行模块或服务其同步回调 | 必须原线程 | 持有目标模块时立即 BUSY |
| `gw_dlopen_enter/leave` | 保持映像存活，允许工作移交 | 可以其他线程 | 按正常关闭等待；不自动判断自等待 |

每次成功进入都增加模块 active；两类归还只能减少各自的借用。scope 的递归进入须逐次归还，不能以一次 leave 清空整个调用链。scope 不支持线程转移、强制取消或线程退出时自动释放；调用方必须在所有正常和错误出口配对归还。

## 状态规则

1. 仅 READY 后端的 LOADING/LIVE 模块接受新借用。隔离或计数耗尽时拒绝，不修改计数。
2. close 在锁内检查当前线程是否持有目标模块；若持有，返回 `GW_ERROR/GW_E_BUSY`，模块状态和资源不变。
3. transport 的 current_module 探测后再次检查 scope，随后才允许 LIVE → CLOSING。保留 current_module 检查以兼容未使用新 scope 接口的旧 transport，但它不能代替祖先借用记录。
4. CLOSING 拒绝新借用，其他线程仍可归还旧借用；最后一次归还唤醒 closer。
5. quarantine 唤醒等待者并禁止 teardown，已有借用仍可归还。它不表示 Guest 已停止执行。

## 存储与上层接入

模块容量固定为 64，因此每个 Host 线程使用连续的 64 项 TLS 数组，按模块槽保存完整代数句柄和递归计数。它记录“持有哪些模块及次数”，不保存调用先后顺序，不是完整调用栈；不分配堆内存，不新增独立调用深度限制。旧代数句柄不能匹配重用后的槽位。

装载层在 transport.find 期间自动持有 scope。JNI 层在 native 跳板执行及注册发布期间持有 scope，涵盖同步 Java 回调和嵌套 Guest 调用；包括 JNI pending exception 在内的返回路径都先归还 scope。当前最内层 module 字段仍用于 RPC/注册归属，不能用它代表整条调用链的借用。

新增 C 符号要求使用包含这些符号的 bionic-dlopen 制品；v1 查询函数表布局和 JNI 共享协议 v4 没有变化。不能只更新头文件并继续链接旧库。

## 仍需调用方保证

这条规则只能阻止**本 Host 线程**等待自己持有的目标模块。它不能证明跨线程等待图无环：线程一持 A 后等待 B，线程二持 B 后等待 A，仍可能死锁。Guest 自建后台线程、缓存函数指针和脱离同步回调链的工作也不会自动登记。

它不解决上下文扩容、注册类所有权、共享依赖归属，也没有 deadline/取消确认机制。宿主仍需同步卸载、停止并 join 后台工作；不能用超时返回释放仍可能被远端访问的栈或 JNI frame。AUD-07 与 AUD-08 因此仍未整体完成。

## 研究依据与区别

[LRPC（1990）](https://graphics.stanford.edu/~tpurcell/quals/os/bers90.pdf) 的线程 linkage stack 提供了显式记录嵌套跨域调用的思路；这里仅借鉴执行链记账，没有移植其内核调度和域终止机制。

[Linux SRCU 规则](https://docs.kernel.org/core-api/kernel-api.html#c.srcu_read_lock) 要求避免直接或间接等待自身仍持有的读侧临界区。这里采用相同的“不等待自己的借用”约束，但继续使用模块锁、计数和条件变量，不是 SRCU 算法，也没有 SRCU 的完整 grace-period 实现。

## 验证入口

- 本项目 CTest：`execution_scopes` 和 `quarantine_0..3`；生产 loader 配合 C transport，适合 ASan/UBSan，不声称建立双 libc。
- `jvm-jni/tests/coverage/run.sh --device SERIAL`：真实 Android JRE/双 libc 下检查自身关闭、A→B 关闭 A/B、A→B→A 关闭 B 及异常展开；结构化 `AUDIT_SCOPE_TEST` 要求四例通过。既有装卸载、并发调用和最终零引用/closure 断言继续运行。
