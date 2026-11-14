# C 装载后端契约

`include/dlopen_backend.h` 是不依赖 JNI 的公开接口。后端只管理 Host 侧逻辑状态，实际 GNU 加载和执行由调用者提供的 transport 在正确的 Guest 域完成。

## 初始化

先通过 `gw_dlopen_configure` 提供 transport、映像解析器和可选事件观察器，再在 Guest 可服务时调用 activate。配置中的函数指针、image_context 和 observer_context 必须保持有效到停止完成。open/find/close 的 `void *` 调用上下文只在该次调用内借用，可以为 NULL；本层不保存 JNIEnv。单例后端当前不支持在一个进程配置多个互相独立的 runtime。

transport 的 load/close/find/current-module 不规定 JavaVM、JNIEnv 或 JNI 描述符；metadata 和绑定说明由上层解释。映像解析器负责路径部署策略。本层不会验证任意库的真实函数原型，也不会自动生成任意 ABI 跳板。

## 公开操作

通过 `gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof(struct gw_dlopen_api_v1))` 获取函数表，检查 NULL、版本和大小。open 返回带代数的逻辑句柄，不能转换为系统 dlopen 指针。find 的返回入口必须满足调用者与 transport 约定的 ABI。

AUTO 根据 ELF GNU 标记分类；显式 GUEST 需要调用者确认 ABI。GW_PASS 允许原平台继续；GW_ERROR 表示 Guest 操作失败，不得用同一 GNU 文件盲目尝试 bionic loader。已经退休的 Guest tag 仍由 owns_handle 识别，避免错误 fallback。

同一设备/inode 不能重复活跃打开。加载失败回滚模块状态；代数让旧句柄在槽位重用后仍被拒绝。固定槽容量是 32，不是无限运行时命名空间。

## 并发和关闭

调用入口必须配对 enter/leave 借用。close 阻止新借用，等待正在执行的调用离开，再通过 transport 卸载。自身活跃回调请求关闭返回 BUSY，避免自等待。后台线程和缓存的入口指针不自动计入借用，调用者必须停止它们并同步卸载。

有模块正在加载、运行或关闭时，stop 不能成功。观察器在后端锁内被调用，只用于轻量计数；不得重入 loader 或执行阻塞工作。错误对象由调用方持有，路径和符号只在单次调用期间借用。

## 验证

CTest 的 `loader_contract` 使用纯 C transport 检查事务、代数、忙碌关闭及停止约束，不宣称独立建立双 libc。真实 ELF 分类测试显式指定输入，不依赖固定上层路径：

```sh
python3 tests/elf_route.py --libraries /path/to/test-dsos \
  --core-include /path/to/core/include --output build/elf-route.json
```

目标 ARM64 输入需设置 `GW_ABI=arm64-v8a`，分类器在桌面 ASan/UBSan 下只解析元数据。真实 Guest 执行与生命周期由集成宿主验证。
