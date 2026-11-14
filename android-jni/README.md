# glibc-worker android-jni

使用公开 JNI API 的通用桥：真实 Host VM 的引用所有权、异常、JNI 代理、线程配对、回调/重入、libffi 跳板，以及 JNI 装载生命周期。JRE 私有钩子不在本项目。

`src/host/`、`src/guest/`、`src/common/` 和 `src/java/` 分别保存 Host、GNU service、共享内部协议与 Java 辅助类；`include/` 保存公开接口。

Host 与 Guest 分别链接 bionic/GNU libc 和各自的 libffi，不在同一线程切换 TLS。Java 辅助类只有 VM 中立的 `BridgeTypes`、`GuestImages`；Guest 的 JavaVM/JNIEnv 是桥接接口。

## 独立构建

安装 core 与 bionic-dlopen 后，用安装前缀列表提供依赖：

```sh
./build.sh -DCMAKE_PREFIX_PATH="/path/to/loader/install;/path/to/core/install" \
  -DGW_JNI_INCLUDE_DIR=/path/to/neutral-jni-header \
  -DGW_FFI_INCLUDE_DIR=/path/to/bionic-libffi/include \
  -DGW_FFI_LIBRARY=/path/to/bionic-libffi/lib/libffi.a \
  -DGW_FFI_LICENSE_FILE=/path/to/bionic-libffi/share/licenses/libffi/LICENSE \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
```

Android 编译还需 NDK toolchain 参数。JDK 只用于生成 Java 8 辅助 JAR。输出 `libgw_android_jni.so/.a`、`android-jni-helpers.jar` 和包 `GlibcWorkerAndroidJni`，目标 `glibc_worker::android_jni`。

GNU 侧由本项目 `build-guest.sh` 单独构建，通过环境变量显式指定 GNU 编译器、GNU libffi、已安装 core/loader 头和 JNI 头。libffi 构建脚本归 core，制品在 `core/build/ffi-{android,linux}/<ABI>/`；本项目通过显式路径消费。工作区组合入口会填入这些参数。

## 可选 VM 适配

`include/jni_vm_adapter.h` 定义安装/恢复钩子表，必须在 bridge 初始化前注册。未请求 `GW_HOST_VM_ADAPTER` 时不调用它；没有注册却请求该 flag 会失败。保留 `GW_HOST_LEGACY_JRE17` 作为相同位的兼容名称。私有实现与固定 profile 属于 jvm-jni。

当前协议、导出名和公开 JNI loader ABI 保留。`host_bridge.h` v2 中的历史 `jre_library_path` 字段仅传给可选适配层；不会被本项目用于推断 JRE 私有布局。

## 测试与验证边界

`tests/native/`、`tests/java/` 保存本项目的 JNI 功能矩阵、引用/异常/线程/重入、SQLite 和数组夹具；由真实 VM 集成宿主组合运行，不复制进 jvm-jni。`tests/check-boundary.py` 可检查已构建 DSO/JAR 没有混入 JRE 私有实现。

已有真实覆盖使用 Android HotSpot；ART 完整 JNI 桥尚未验证，不能把“使用公开 JNI”当作 ART 兼容证明。设计与使用约束见 [装载接口](docs/LOADER_BACKEND.md)，数组副本缓存见 [ARRAY_COPY_CACHE](docs/ARRAY_COPY_CACHE.md)。

Host binding、引用/ID 和注册类记录使用 [Zig 存储](docs/ZIG_BINDINGS.md)，构建需要 Zig 0.16.0。初始容量分别为 512、16,384、256，按 64 槽稳定分块扩容；旧 C 定容后端已移除。模块固定上限为 64。公共 ABI 和 libffi/JNI 执行由 C 薄层维护，Host/Guest 必须成套部署。

线程绑定、动态上下文/重入帧、类注册交接及依赖函数归属见 [JNI 交互契约](docs/CONTEXTS_AND_OWNERSHIP.md)。

每进程一套 Runtime 绑定一个目标 VM；关闭与隔离后均不可重建。所有者线程、正常关闭顺序、超时不能提前释放在途资源，以及宿主监督责任见 [生命周期与进程处置](docs/LIFECYCLE.md)。
