# C 装载后端契约

`include/dlopen_backend.h` 是不依赖 JNI 的公开接口。后端只管理 Host 侧逻辑状态，实际 GNU 加载和执行由调用者提供的 transport 在正确的 Guest 域完成。

## 初始化

先通过 `gw_dlopen_configure` 提供 transport、映像解析器和可选事件观察器，再在 Guest 可服务时调用 activate。配置中的函数指针、image_context 和 observer_context 必须保持有效到停止完成。open/find/close 的 `void *` 调用上下文只在该次调用内借用，可以为 NULL；本层不保存 JNIEnv。单例后端当前不支持在一个进程配置多个互相独立的 runtime。

transport 的 load/close/find/current-module 不规定 JavaVM、JNIEnv 或 JNI 描述符；metadata 和绑定说明由上层解释。映像解析器负责路径部署策略。本层不会验证任意库的真实函数原型，也不会自动生成任意 ABI 跳板。

## 公开操作

通过 `gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof(struct gw_dlopen_api_v1))` 获取函数表，检查 NULL、版本和大小。open 返回带代数的逻辑句柄，不能转换为系统 dlopen 指针。find 的返回入口必须满足调用者与 transport 约定的 ABI。

AUTO 根据 ELF GNU 标记分类；显式 GUEST 需要调用者确认 ABI。GW_PASS 允许原平台继续；GW_ERROR 表示 Guest 操作失败，不得用同一 GNU 文件盲目尝试 bionic loader。已经退休的 Guest tag 仍由 owns_handle 识别，避免错误 fallback。

同一设备/inode 不能重复活跃打开。安全完成的加载失败回滚模块状态；代数让旧句柄在槽位重用后仍被拒绝。固定槽容量是 64，不是无限运行时命名空间。

## 并发和关闭

执行模块代码或服务其同步回调时，必须配对 `gw_dlopen_scope_enter/leave`。scope 归当前 Host 线程所有，成功进入后须在同一线程、线程退出前归还；失败进入不能调用 leave。递归进入按次数归还。关闭当前线程持有的任意祖先模块返回 `GW_E_BUSY`，拒绝发生在进入 CLOSING 之前，不调用 teardown、不隔离桥。`find` 在 transport 查找期间自动持有 scope；返回的入口不会因此永久保活。

`gw_dlopen_enter/leave` 则提供可跨线程归还的保活 pin，不表示执行归属。两类借用分别记账，不能用一种 leave 消耗另一种借用；scope_leave 对陈旧、未持有或其他线程的 scope 返回 0，不修改计数。pin 持有者若自行 close，仍会等待 pin 归还；应先归还或确保其他线程会归还，不能把 pin 当作自等待保护。

close 阻止新借用，等待其他线程持有的 scope/pin 归还，再通过 transport 卸载。该规则不检查跨线程等待环，也不会自动发现后台线程和缓存入口的使用；调用者必须停止它们并同步卸载。完整规则、适用边界和研究来源见 [执行借用](EXECUTION_SCOPES.md)。

有模块正在加载、运行或关闭时，stop 不能成功。观察器在后端锁内被调用，只用于轻量计数；不得重入 loader 或执行阻塞工作。错误对象由调用方持有，路径和符号只在单次调用期间借用。

## 不可恢复的清理失败

transport 无法确认回滚或卸载安全时，调用 `gw_dlopen_quarantine()`。该状态不可撤销，`gw_dlopen_is_quarantined()` 可供宿主检查；本层不会终止进程。宿主必须停止使用该 Guest 桥并丢弃承载它的进程，不能在同进程重新配置或重试恢复。

隔离后 Guest open/find/close、activate/configure/stop 返回 `GW_ERROR` 和 `GW_E_QUARANTINED`，enter 拒绝新借用。等待借用结束的 close 会被唤醒并退出，不再调用 teardown。出错事务保留 LOADING 或 CLOSING 槽及资源，不复用句柄；已有借用仍可 leave。普通平台句柄仍可 PASS，隔离不会把 GNU 库改交给 bionic。

transport 仍须负责保留可能被 VM 或 Guest 线程引用的映像、入口和上下文。隔离不是终止后台线程、取消在途调用或阻止任意原生代码执行的沙箱；后台线程与缓存入口的同步要求仍然成立。

## 验证

CTest 的 `loader_contract` 使用纯 C transport 检查事务、代数、忙碌关闭及停止约束；`execution_scopes` 检查祖先/递归关闭、64 模块借用、线程归属、pin 与 scope 隔离、句柄代数及正常跨线程关闭；`quarantine_0..3` 检查失败装载、失败关闭以及 pin/scope 等待关闭中的终态隔离和资源保留。它们不宣称独立建立双 libc。真实 ELF 分类测试显式指定输入，不依赖固定上层路径：

```sh
python3 tests/elf_route.py --libraries /path/to/test-dsos \
  --core-include /path/to/core/include --output build/elf-route.json
```

目标 ARM64 输入需设置 `GW_ABI=arm64-v8a`，分类器在桌面 ASan/UBSan 下只解析元数据。真实 Guest 执行与生命周期由集成宿主验证。
