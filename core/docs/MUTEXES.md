# GNU mutex 支持契约

本契约针对本项目构建的 GNU runtime。Host 的 bionic mutex 不受此补丁影响，两套 libc 的 pthread 对象仍不能混用。

## 支持与拒绝

普通 non-robust mutex 保留 glibc 实现。专项回归覆盖 normal 加锁/解锁、trylock 的 EBUSY、另一线程 timedlock 的超时、recursive 重入、errorcheck 的 EDEADLK，以及真实条件变量等待/唤醒。

`pthread_mutex_init` 收到 `PTHREAD_MUTEX_ROBUST` 属性时返回 **ENOTSUP**，在写入 mutex 对象前拒绝。`pthread_mutexattr_setrobust` 仍可接受该属性；属性配置成功不表示 mutex 创建成功。拒绝初始化的对象不能继续 lock/unlock/destroy。该规则不因 private/shared、normal/recursive/errorcheck 或优先级协议改变；测试矩阵覆盖这三类锁、两种共享属性和 NONE/INHERIT 协议，共 12 种组合。shared 属性检查不等于跨进程同步验证。

这是明确的兼容性限制：依赖 robust owner-death 恢复的原生库必须正确处理 ENOTSUP 或拒绝启动。桥不把 robust 属性改成普通锁，也不模拟恢复成功。其他 mutex 属性、PI 行为及跨进程场景没有因这些检查通过而获得完整兼容保证。

## 原因

Termux NPTL 补丁关闭 `set_robust_list` 登记。当前 ARM64 glibc 的 `__ASSUME_SET_ROBUST_LIST` 仍为真，正常 pthread 退出路径中的用户态 fallback 因而没有编入。单独删除登记调用，却继续让 robust mutex 创建成功，会使调用者得不到预期的 owner-death 通知。

旧制品已在普通 Android 应用的同进程 GNU Guest 中复现：12 种配置均创建成功；持锁线程从 pthread 入口正常返回后，下一线程 timedlock 返回 ETIMEDOUT，未获得 EOWNERDEAD。该实验使用有限等待，没有通过无限阻塞猜测结果。主机标准 glibc 的同一夹具验证 EOWNERDEAD、consistent 后复用，以及未修复就解锁后的 ENOTRECOVERABLE，作为用例正对照。

标准恢复流程可参考维护者的 [Linux pthread_mutexattr_setrobust 手册](https://man7.org/linux/man-pages/man3/pthread_mutexattr_setrobust.3.html)。本 runtime 采用显式拒绝策略；正常退出的用户态补偿不能替代完整内核登记语义。

补丁 [0003-reject-robust-mutex.patch](../patches/0003-reject-robust-mutex.patch) 在 glibc 内部初始化入口检查属性位，因此同一实现的 C ABI 别名也经过该检查；没有新增符号转发表。GNU loader/libc 必须来自同一匹配构建并成套更新，见 [构建契约](RUNTIME_PATCHES.md)。它不改变 JNI 私有协议 v4。

## 复现

从工作区根目录执行；需要显式选择真实存在的工具链。以下 ARM64 示例要求 GNU 交叉编译器与 QEMU 已安装：

```sh
GW_ABI=arm64-v8a core/scripts/build-glibc.sh
python3 core/tests/mutex_contract.py \
  --runtime core/build/glibc-runtime/arm64-v8a \
  --guest-cc aarch64-linux-gnu-gcc --loader ld-linux-aarch64.so.1 \
  --emulator qemu-aarch64
```

脚本先用 ASan/UBSan 执行主机标准 glibc 正对照，再由指定 loader/libc 执行 GNU 夹具。省略 runtime 参数只运行主机正对照；QEMU 结果不代表 Android 或双 libc 验证。`summary.json` 记录运行方式、制品散列和结构化结果。

真实 Android 测试将同一纯 C 夹具链接进 core 的 Guest probe；smoke/coverage 都要求唯一 `MUTEX_CONTRACT` JSON，确认 12 种 robust 配置拒绝且普通锁检查通过。组合测试与 APK 的所有权仍归 jvm-jni。若刚刚替换了预构建 GNU runtime 并使用 `GW_DEPS_READY=1`，应强制重跑准备/打包任务并核对 APK 内制品散列，不能使用旧 APK 的 PASS。

专项检查不覆盖 SIGKILL、任意信号终止、raw SYS_exit、进程崩溃或完整 pthread 规范；显式拒绝 robust 初始化使本项目不再为这些恢复路径宣称能力。
