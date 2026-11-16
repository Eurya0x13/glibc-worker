# Project boundaries and review order

[English](../en/REVIEW_GUIDE.md) | [简体中文](../zh/REVIEW_GUIDE.md)

Contracts belong to their owning project:

- core: [runtime patches](../../core/docs/en/RUNTIME_PATCHES.md), [mutexes](../../core/docs/en/MUTEXES.md).
- bionic-dlopen: [execution borrows and close](../../bionic-dlopen/docs/en/EXECUTION_SCOPES.md).
- android-jni: [capacity/growth](../../android-jni/docs/en/ZIG_BINDINGS.md), [contexts/ownership](../../android-jni/docs/en/CONTEXTS_AND_OWNERSHIP.md), [lifecycle](../../android-jni/docs/en/LIFECYCLE.md), [failures](../../android-jni/docs/en/FAILURES.md).
- jvm-jni: [adapters](../../jvm-jni/docs/en/ADAPTER.md), [JRE compatibility/limits](../../jvm-jni/docs/en/JRE_COMPATIBILITY.md).

1. Review core bootstrap, raw clone, futex events and diagnostics. Ensure there are no JNI, Java or libffi dependencies.
2. Review bionic-dlopen C transport, ELF classification, logical handles and load/unload synchronization. JNI versions and method descriptors are not interpreted here.
3. Review android-jni forwarding, references, exceptions, threads, callbacks and loading adaptation. VM extensions use an optional hook table.
4. Review jvm-jni, the only project aware of NativeLibraries and pinned libjava profiles, plus real Android JRE integration hosts.

Each project owns its CMake package, build entrypoints, tests and documentation. Dependencies flow through installed artifacts; lower layers do not include upper-layer private headers. The root owns navigation, composition, formatting and packaging only.

Retained JNI v1/v2 interfaces and shared protocols preserve their declared compatibility. Single-file `libglibc_bridge.so` is jvm-jni compatibility composition, not the definition of core.

Core and bionic-dlopen run local tests through their own CTest. See the [integration manual](../../jvm-jni/tests/README.md) for Android/JRE reproduction and pass criteria. Results belong in project build directories, not developer interface documentation.
