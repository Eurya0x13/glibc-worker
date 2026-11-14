# 工作区工具

`build-runtime.sh` 按依赖顺序调用各项目的 `build.sh`，不拥有公共构建目录。`target.py` 为组合器发现 NDK 并复用 core 的目标 profile；`format.py` 只格式化维护源码；`export-source.py` 分别导出四个项目。

GNU loader/libffi 构建工具属于 `core/scripts/`，JNI 头与转发表生成、GNU JNI service 打包属于 `android-jni/scripts/`。各项目的 `scripts/package-runtime.sh` 负责自己的兼容布局，合并 JVM DSO/JAR 由 `jvm-jni/scripts/` 负责。JRE 制品和 Android 测试构建属于 `jvm-jni/tests/scripts/`。
