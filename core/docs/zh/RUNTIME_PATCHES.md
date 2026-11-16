# GNU runtime 补丁构建契约

[English](../en/RUNTIME_PATCHES.md) | [简体中文](../zh/RUNTIME_PATCHES.md)

runtime 必须使用同一构建产生的 GNU loader/libc。`scripts/build-glibc.sh` 的缓存键包含构建脚本、补丁应用器、目标配置和补丁正文；修改这些输入会选择新的源码/对象目录。

核对发生在调用该 glibc 构建脚本时。组合构建显式设置 `GW_DEPS_READY=1` 会直接消费现有制品，调用方应已确认它们来自已验证的匹配构建。

补丁应用由 `scripts/apply-patches.py` 完成。它将源码路径解析为真实路径，清除继承的 Git 工作树/索引环境，并在源码父目录设置 Git discovery ceiling。这样源码位于父级 Git checkout 的忽略目录时，补丁仍针对独立源码树处理，不受调用目录 prefix 影响。

不能仅用 `git apply` 的退出码证明补丁已经应用：在包含 Git 仓库的子目录运行、同时给出绝对 `--directory` 时，Git 可以跳过所有路径并返回 0。构建器必须在每次应用后验证反向补丁可匹配，成功后才写 `patched` 标记；复用缓存时也重新验证。验证失败立即停止，不继续编译或发布 runtime。

这会直接影响 Android 兼容性：例如缺失 NPTL 补丁会使 loader 的 `__tls_init_tp` 执行 `set_robust_list`，ARM64 上为系统调用 99，可能在普通应用的 seccomp 策略下触发 `SIGSYS`。拒绝发生在 Guest 启动阶段，不是 JNI 容器容量检查。

```sh
python3 core/tests/runtime_patches.py
```

回归包含源码位于 Git 忽略目录及独立源码目录两种布局，覆盖污染的 `GIT_DIR/GIT_WORK_TREE` 环境、正向应用、反向核对、重复应用拒绝和损坏缓存拒绝。测试只修改 core 的 build 临时目录，不要求下载或重建 glibc。

补丁正确应用不消除其兼容性取舍。关闭 robust-list 登记后，`0003-reject-robust-mutex.patch` 使 robust mutex 初始化明确返回 ENOTSUP；普通 non-robust 锁保留 glibc 实现。支持范围、旧行为复现和专项测试见 [mutex 契约](MUTEXES.md)。
