# glibc-worker workspace

[English](docs/en/README.md) | [简体中文](docs/zh/README.md)

Run an independent GNU libc domain inside an Android bionic process. This workspace contains four independent projects; Java/HotSpot integration is optional.

| Project | Responsibility | Dependencies |
| --- | --- | --- |
| [core](core/docs/en/README.md) | Bootstrap a pinned GNU loader, architecture entrypoints, events and RPC diagnostics | Linux/Android interfaces; no JNI/Java |
| [bionic-dlopen](bionic-dlopen/docs/en/README.md) | C loading contract, ELF routing, handle ownership and load/unload synchronization | core; no JNI/Java |
| [android-jni](android-jni/docs/en/README.md) | Public JNI Host/Guest bridge, references, exceptions, callbacks and GNU service | core, bionic-dlopen, libffi for both domains |
| [jvm-jni](jvm-jni/docs/en/README.md) | Optional private HotSpot/JRE loading adapters and real JRE integration tests | android-jni |

Each project keeps implementation in `src/`, public interfaces in `include/`, and tests in `tests/`. Each owns its CMake build, `build.sh`, documentation and `build/` outputs. Dependencies use installed CMake packages and headers. There is no shared build or test directory.

`android-jni/build.zig` compiles the required Zig Host stores. CMake still owns inter-project dependencies, C/JNI compilation and SDK installation; the four builds have not all migrated to Zig.

Root `scripts/` provides workspace composition, toolchain discovery, formatting and export. It invokes project build entrypoints while leaving artifacts with their owners:

```sh
GW_ABI=arm64-v8a scripts/build-runtime.sh --module core
GW_ABI=arm64-v8a scripts/build-runtime.sh --module bionic-dlopen
GW_ABI=arm64-v8a scripts/build-runtime.sh --module android-jni
GW_ABI=arm64-v8a scripts/build-runtime.sh --module jvm-jni
```

Selecting an upper layer builds its dependencies in order. Individual `build.sh` entrypoints are also available; see the project guides and [workspace builds](docs/en/BUILD.md).

The jvm-jni composition additionally provides the single `libglibc_bridge.so` and helper JAR used by legacy embedders. This upper-layer compatibility artifact is not core and must not coexist with the modular DSOs in one process.

Real validation primarily covers Android HotSpot. Separating public JNI from private JRE interfaces does not prove full ART compatibility. Pojav and real-mod integration live in external validation projects; this workspace contains no launcher source.

Downloads, source caches and outputs belong to each project's `vendor/`, `work/` and `build/`. The workspace root keeps none of these caches. See [style](docs/en/STYLE.md) and [review order](docs/en/REVIEW_GUIDE.md).

Planned work is tracked in the [project TODO](docs/en/TODO.md).

## Same-thread domains

Core maintains context switching, adoption patches and signal routing. The
`runtime_bridge.h` startup API in android-jni defaults to same-thread execution.
Embedders can explicitly select the worker backend for compatibility. It is
in maintenance mode. Each project keeps its same-thread fixtures under
`tests/same-thread/`, generated outputs under `build/tests/`, and reviewed
historical captures under `tests/results/`. See
[domain and signal boundaries](core/docs/en/DOMAINS.md) and
[runtime integration](android-jni/docs/en/RUNTIME.md).

The [measurement guide](jvm-jni/docs/en/PERFORMANCE.md) describes the fixed
workloads and calculation methods. The
[1–2 MiB cache results](android-jni/tests/results/same-thread/array-cache/README.md)
cover array copying and allocator page return. The
[array-copy contract](android-jni/docs/en/ARRAY_COPY_CACHE.md) describes caller
obligations. Results apply to the pinned ART, JRE and mod artifacts tested.
