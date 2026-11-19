# Project TODO

[English](../en/TODO.md) | [简体中文](../zh/TODO.md)

- [ ] Evaluate targeted C-to-Zig migrations on `experiment/zig-bindings`.
  Start with a self-contained component that can benefit from Zig's type
  system and explicit resource management. Preserve the public C ABI and
  compare correctness, build requirements and performance with the existing
  implementation before expanding the migration.

- [ ] Verify glibc alias staging in an installed Android x86_64 APK.
  An installed ARM64 APK verified app-private staging and a cold Guest load
  of a module whose only dependency was `libdl.so.2`. The packaged loader and
  staging helper also passed on Linux x86_64 and emulated ARM64.
  [Runtime integration](../../android-jni/docs/en/RUNTIME.md#packaged-glibc-library-names)
