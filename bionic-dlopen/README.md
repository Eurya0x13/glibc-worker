# glibc-worker bionic-dlopen

JNI 无关的 Host 装载层：ELF GNU 标记分类、映像路由、带代数的逻辑句柄、重复 inode 拒绝、借用计数和关闭同步。

`src/` 保存 C 装载器与 ELF 分类实现；`include/` 保存公开装载契约。

## 独立构建

先安装 core，再构建本项目；只读取 core 的安装包，不读取其源码或测试：

```sh
./build.sh -DCMAKE_PREFIX_PATH=/path/to/core/install \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
ctest --test-dir build --output-on-failure
```

输出 `build/libgw_dlopen.so`、`build/libgw_dlopen.a` 与包 `GlibcWorkerDlopen`，导出目标 `glibc_worker::dlopen`。不查找 Java、JNI 头或 libffi。

## 接口

公开接口为 [dlopen_backend.h](include/dlopen_backend.h)，生命周期和并发要求见 [C 装载契约](docs/LOADER.md)。调用者配置 C `transport`、映像解析器和可选观察回调，再 activate 后使用版本化函数表。

调用上下文为 `void *`，纯 C 客户端可传 NULL；metadata 是适配层的 int32 结果，不由本层解释。符号绑定说明也由适配层解释，JNI 描述符和 JNI_OnLoad 版本校验均不在这里。

本项目不会在 bionic 线程直接执行 GNU 符号，也不重写 ELF loader。实际 GNU dlopen/调用/关闭由 transport 在正确的 Guest 线程域完成，返回入口的 ABI 与生命周期由适配层保证。它不是进程内全局 dlopen 劫持。

AUTO 路由只在 ELF 元数据成功解析、没有 GNU 标记时返回 `GW_PASS`。无法读取、损坏或不支持的映像返回 `GW_ERROR/GW_E_IMAGE`；分类器资源上限返回 `GW_ERROR/GW_E_CAPACITY`。两类失败均不允许交给原平台 loader 重试。显式 `GW_OPEN_GUEST` 由调用方确认 GNU 域，跳过自动分类。

分类器保留有界资源：128 个 program headers、512 个 dynamic entries、256 字节名字（含终止符）、64 个版本依赖组、每组 256 个版本。超限是显式失败，不是“非 glibc”。元数据分类也不代替真正 loader 的重定位、符号与 ELF 完整性检查。

## 测试

`tests/loader_contract.c` 使用普通 C transport 验证 NULL 上下文、非 JNI metadata、查找/调用、重复映像、失败回滚、旧句柄、忙碌关闭和停止约束。它验证 Host 契约，不宣称单独建立双 libc。

`tests/execution_scopes.c` 验证祖先/递归 scope、线程归属和正常跨线程关闭；`close_waits.c` 验证两/三线程的 scope 关闭环拒绝以及无活跃借用目标关闭；`loader_quarantine.c` 覆盖四种隔离场景。执行借用与可转移保活 pin 的区别见 [执行借用规则](docs/EXECUTION_SCOPES.md)。

`tests/elf_route.py` 在 ASan/UBSan 下检查 49 项真实 Android/GNU ELF、无标记映像、损坏和资源上限条件，同时核对分类与错误码；该集成夹具可消费上层测试产生的 DSOs。C contract 另外确保 AUTO 分类失败不会返回 PASS 或调用 Guest transport。真实双 libc 生命周期由 jvm-jni 的集成测试覆盖。
