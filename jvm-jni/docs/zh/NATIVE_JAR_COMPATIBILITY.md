# 原版 GNU JNI JAR 兼容性验证

[English](../en/NATIVE_JAR_COMPATIBILITY.md) | [简体中文](../zh/NATIVE_JAR_COMPATIBILITY.md)

## 目标和固定输入

覆盖 JAR 原生资源选择、System.load 路径、GNU DT_NEEDED 依赖、实际数据处理与资源生命周期。直接使用上游未修改的 JAR/ELF，不要求作者重编译，也不把纯 Java 模组算作 GNU 兼容覆盖。

| 制品 | 固定版本 | 主要覆盖 |
| --- | --- | --- |
| zstd-jni | 1.5.7-6 | 压缩/解压、错误路径、流、原生线程和两次加载生命周期 |
| lz4-java | 1.8.0 | 数组、DirectBuffer、流和并发 |
| snappy-java | 1.1.10.12 | 原生 codec、流、校验和与错误输入 |
| rocksdbjni | 10.2.1 | 数据库读写、列族、直接缓冲区、并发与重开 |

JRE/zstd 的散列位于 `tests/config/targets.json`；额外 JAR、source JAR 及每 ABI 原生资源的散列位于 `tests/config/native-jars.json`。第三方许可归 `tests/third_party/`，下载缓存归本项目 `vendor/`。

## 依赖闭包和映像部署

`tests/scripts/prepare-native-jars.py` 验证 ELF 架构、SONAME、DT_NEEDED 与散列。glibc 的 math/rt/dl 使用 core 发布的匹配 runtime；GNU libgcc/libstdc++ 通过所选编译器发现，并必须匹配 `tests/config/native-jars-runtime.json`。不匹配时停止，不能静默接受任意本机库。

Android 按 lib*.so 部署映像。Guest loader 按绝对安装路径预加载依赖，再通过原始 SONAME 解析。JAR 仍自行解包资源并调用 System.load；公共内容策略找到已安装的相同字节映像。测试不设置 assume-loaded 或 native-path 绕过，也不维护函数符号白名单。

## 执行

从工作区根目录运行：

```sh
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar lz4
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar snappy --reuse-apk
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar rocksdb --reuse-apk
```

首次构建启用 `GW_NATIVE_JARS=1`；runner 会为 `--native-jar` 设置它。每个库在新的应用测试进程中运行，先完成基础 JNI/SQLite/zstd 覆盖，再执行所选库的完整数据与生命周期检查。不要并行争用同一个测试包。

## 通过标准和限制

结果必须包含普通应用 UID、正确 JRE/ABI、未修改制品散列、全部功能断言、两次资源生命周期、类加载器逻辑卸载、JNI 引用清理、Guest 停止和 JVM 销毁。超时、CheckJNI 违规、错误数据或退出不完整均为失败。具体检查由 `tests/check.py` 执行，结果位于 `build/test-results/`。

逻辑卸载不保证映像物理消失，GNU UNIQUE 等上游行为可能保留 DSO。RocksDB 关闭数据库也不保证其进程级后台线程池立即退出。必须分别记录资源所有权、线程和映射，不能把它们混为泄漏或完全卸载的证明。

库级验证不代表整个 Minecraft 模组通过。后续验证应按单模组功能/保存退出，再按固定多模组场景测试；性能问题只在能复现的真实路径上继续定位。
