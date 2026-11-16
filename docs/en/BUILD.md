# Workspace builds

[English](../en/BUILD.md) | [简体中文](../zh/BUILD.md)

The four projects independently own CMake builds, installed packages, tests and build directories. Their guides describe standalone builds; workspace scripts only compose dependency order and legacy embedder artifacts.

```sh
GW_ABI=arm64-v8a scripts/build-runtime.sh --module core
GW_ABI=arm64-v8a scripts/build-runtime.sh --module bionic-dlopen
GW_ABI=arm64-v8a scripts/build-runtime.sh --module android-jni
GW_ABI=arm64-v8a scripts/build-runtime.sh --module jvm-jni
```

The default `all` matches `jvm-jni`. Objects, libraries, generated headers, helper JARs and CMake installations stay under `<project>/build/<ABI>/`. Dependencies come through each project's `install/`; there is no shared installation prefix or upper-layer source compiled into a lower layer.

GNU runtime artifacts use `core/build/glibc-runtime/<ABI>/`; libffi uses `core/build/ffi-{android,linux}/<ABI>/`. JNI generation belongs to `android-jni/build/`; Android applications, JREs and integration results belong to `jvm-jni/build/`. No root build/work/vendor directories are retained. Formatter tools use `scripts/build/tools/`.

Composed core/bionic-dlopen builds need Python, CMake and NDK, but do not invoke javac, read JNI headers or build libffi/JRE. JNI layers additionally need a JDK, matching GNU toolchain and glibc/libffi. Discover NDK through environment configuration or `sdk.dir` in workspace local.properties; do not guess installation paths.

Defaults are initial `GW_CONTEXTS=16` (build range 2..256; runtime growth in groups of 16), `GW_RPC_PROFILE=0`, `GW_RPC_SPIN_NS=20000`, `GW_FUTEX_WAKE_FILTER=0` and `GW_RPC_READY_POLL=0`. Each project keeps its own abi-flags; Host and Guest must use matching layout/configuration.

Host stores require exactly Zig 0.16.0 on the build machine; installed C/C++ SDK consumers do not need Zig. Omit `GW_BINDING_BACKEND` or set `zig`; `c` is rejected. Clear an old CMake cache containing `c` or change it to `zig`. Initial capacities are 512 bindings, 16,384 reference/ID slots and 256 class records, with dynamic growth. Module capacity stays fixed at 64. See [Host stores](../../android-jni/docs/en/ZIG_BINDINGS.md).

`GW_JRE_ADAPTER_HEADER` applies only to jvm-jni and must select a profile matching the running libjava. Without configuration, private hooks cannot be requested. `GW_RUNTIME_OUT` redirects only upper-layer compatibility packaging, not project compilation/installation.

Legacy single-DSO integration consumes `libglibc_bridge.so`, GNU service, helpers and bootstrap from `jvm-jni/build/<ABI>/compat/`; modular integration consumes the four installed packages. Do not load both compositions in one process and duplicate singleton state.

Real VM smoke/coverage belongs to [jvm-jni](../../jvm-jni/docs/en/README.md). Pojav integration is outside the core projects.

## Cleaning generated caches

Preview obsolete glibc source/object caches, then apply the same policy:

```sh
python3 scripts/clean-cache.py
python3 scripts/clean-cache.py --apply --json core/build/cleanup-result.json
```

The cleaner retains the current ordinary runtime sources and the most recently
published domain runtime source generation for each ABI. Use
`--keep-domain-builds 2` to retain two domain generations per ABI, or
`--keep core/work/<build-directory>` to protect a specific source tree. Only
recognized, Git-ignored caches under `core/work/` are eligible. Tracked files,
active build trees and symlink roots are refused. Run between builds; this tool
cannot prevent an unrelated build from starting after its process check.

Published runtime binaries, libffi, JREs, downloaded dependencies and test
results are retained. Removing old source/object trees can make rebuilding an
old runtime slower. The JSON report records each completed deletion, including
partial progress if cleanup fails. Running without `--apply` deletes nothing.

## Source layout and build migration

Implementations live in `core/src/bootstrap/`, `bionic-dlopen/src/`, `android-jni/src/{host,guest,common,java}/` and `jvm-jni/src/{native,java}/`. Public JVM adapter headers use `jvm-jni/include/`; internal relocation does not change installed include paths. Generated sources go into project build directories, never back into src.

`android-jni/build.zig` is the sole Zig compilation graph. CMake and test runners invoke `zig build host-store`; Python does not duplicate compiler options. Future migration can follow each project's compile/link/install boundaries. Dependency fetching, JNI table generation, device testing and reporting need not become Zig merely because the build graph changes.
