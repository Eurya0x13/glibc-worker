# glibc-worker jvm-jni

可选的 HotSpot/JRE 接入项目。按经过审阅的装载接口族接入 JRE8–25，维护私有装载钩子、方法签名解析和 libjava 制品校验；通过 android-jni 注册表接入，不被下层反向引用。

JRE8、11、17、21、25 已通过真实桌面 VM 的适配器契约测试；Android 双 libc 完整覆盖目前仍以固定 JRE17 为准。装载适配不等于所有 JNI 新槽位或所有 Android JRE 制品均已验证，具体边界见 [兼容性契约](docs/JRE_COMPATIBILITY.md)。JDK28 不在运行支持范围。

## 独立构建

```sh
./build.sh -DCMAKE_PREFIX_PATH="/path/to/android-jni/install;/path/to/loader/install;/path/to/core/install" \
  -DGW_JNI_INCLUDE_DIR=/path/to/neutral-jni-header \
  -DGW_JRE_ADAPTER_HEADER=/path/to/reviewed-jre-profile.h \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
```

输出 `build/libgw_jvm_jni.so/.a`、`jvm-jni-helpers.jar` 和包 `GlibcWorkerJvmJni`。JAR 仅编译本项目的 `JvmAdapter` 与兼容入口 `Jre17Adapter`，通过已安装的 android-jni helper JAR 引用公共类。未指定构建 profile 时生成 unconfigured 配置，启用前必须通过 `gw_jvm_jni_set_profile` 提供审阅后的 profile；不会自动按 JRE 主版本放行。

加载本项目 DSO 只注册可选适配器，不立即修改 VM。宿主在初始化时明确请求 `GW_HOST_VM_ADAPTER` 后才安装，停止时恢复。必须使用已运行的同一 VM 的 JNIEnv。

profile API、六个接口族、构建配置及停止条件见 [适配器接入](docs/ADAPTER.md)。

## 集成测试

本项目 `tests/` 拥有 Android/JRE 测试应用、JRE 制品配置、JRE 宿主和集成 runner；显式引用 android-jni 的 JNI 夹具以及 core 的 POD 夹具。

从工作区根目录运行：

```sh
GW_ABI=arm64-v8a jvm-jni/tests/android/gradlew -p jvm-jni/tests/android :app:assembleDebug
jvm-jni/tests/smoke/run.sh --device SERIAL
jvm-jni/tests/coverage/run.sh --device SERIAL
```

输出全部在本项目 `build/`。冒烟与覆盖是同一上层集成的两组测试，不再放在公共 tests 目录；底层事件和 C loader 测试分别归 core 与 bionic-dlopen。

主机多 JRE 测试不需要 Android 设备，入口为 `tests/scripts/test-adapters.py`。它在真实 VM 中替换生产装载钩子，验证生命周期、ClassLoader 回收和失败回滚，传输使用主机自身的 libc。固定制品及可复现命令见 [测试手册](tests/README.md)。

工作区组合构建额外在本项目 `build/<ABI>/compat/` 生成旧宿主所需 `libglibc_bridge.so`、GNU service、辅助 JAR 和旧平面头布局。该单 DSO 与模块化 DSOs 二选一，不可在同一进程混装。

既有真实 JNI JAR 兼容性、延迟与统计记录归本项目 [docs](docs/)。Pojav 接入代码仍在外部验证工程。长期 GC、完整运行时销毁/重建、任意 C++/信号共存等边界仍需独立验证。
