# 项目待办

[English](../en/TODO.md) | [简体中文](../zh/TODO.md)

- [ ] 在 `experiment/zig-bindings` 上评估将部分 C 实现迁移到 Zig。
  先选择一个适合利用 Zig 类型系统和显式资源管理的独立组件。保留公开 C ABI，
  对照现有实现的正确性、构建要求和性能，再决定是否扩大迁移范围。
