# 项目待办

[English](../en/TODO.md) | [简体中文](../zh/TODO.md)

- [ ] 在 `experiment/zig-bindings` 上评估将部分 C 实现迁移到 Zig。
  先选择一个适合利用 Zig 类型系统和显式资源管理的独立组件。保留公开 C ABI，
  对照现有实现的正确性、构建要求和性能，再决定是否扩大迁移范围。

- [ ] 在两个 Android ABI 的已安装 APK 中验证 glibc 别名部署。
  已在 Linux x86_64、模拟 ARM64 和 ARM64 真机 ART shell 中验证打包 loader 和别名辅助类。
  APK 回归需在启动 Guest runtime 时覆盖打包部署和应用私有目录。
  [运行时接入](../../android-jni/docs/zh/RUNTIME.md#打包后的-glibc-库名)
