# Project TODO

[English](../en/TODO.md) | [简体中文](../zh/TODO.md)

- [ ] Evaluate targeted C-to-Zig migrations on `experiment/zig-bindings`.
  Start with a self-contained component that can benefit from Zig's type
  system and explicit resource management. Preserve the public C ABI and
  compare correctness, build requirements and performance with the existing
  implementation before expanding the migration.

- [ ] Verify glibc alias staging in an installed APK on both Android ABIs.
  The packaged loader and staging helper passed on Linux x86_64 and emulated
  ARM64. An ART shell check also passed on an ARM64 Android device. Exercise
  the APK packaging and app-private paths at runtime startup.
  [Runtime integration](../../android-jni/docs/en/RUNTIME.md#packaged-glibc-library-names)
