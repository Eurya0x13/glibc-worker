# glibc-worker core

[English](docs/en/README.md) | [简体中文](docs/zh/README.md)

Runtime foundations without JNI, Java or libffi dependencies: pinned GNU loader PT_LOAD mapping and startup stack construction, architecture-specific raw clone entrypoints, futex events and RPC diagnostics. The existing GNU loader still performs DSO relocation and TLS initialization.

`src/bootstrap/` contains loader startup, `src/domain/` context switching and signal routing, `src/runtime/` GNU adoption patches, and `include/` protocol/runtime primitives.

## Standalone build

Run from the core project directory:

```sh
./build.sh -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
ctest --test-dir build --output-on-failure
```

Outputs stay in this project's `build/`. Select Android through the CMake NDK toolchain, `ANDROID_ABI` and `ANDROID_PLATFORM`; no JDK, JNI headers or other project's source is required. The `GlibcWorkerCore` package exports `glibc_worker::core`, `glibc_worker::domains` and opt-in `glibc_worker::signal_interpose`. See [same-thread domains](docs/en/DOMAINS.md).

`scripts/build-glibc.sh` builds matching GNU loader/libc artifacts; `scripts/build-ffi.sh /path/to/ndk-clang` builds Host/Guest libffi. These optional dependency tools do not add link dependencies to the base core library. Configuration and Termux patches belong to core. Downloads use `vendor/`, sources use `work/`, runtime artifacts use `build/glibc-runtime/<ABI>/`, and libffi uses `build/ffi-{android,linux}/<ABI>/`. Runtime packaging publishes matching loader, libc, libpthread, libm, librt and libdl; callers do not inspect glibc's internal build tree.

## Tests and boundaries

`tests/` owns real futex, notification race, paired handoff, nested statistics and controlled clock-failure checks. They require Python and Clang ASan/UBSan; outputs use `build/test-work/`. Use `-DBUILD_TESTING=OFF` to build only components.

`gw_bootstrap_stack` accepts 1..16 non-null argument pointers. Before writing, it checks address overflow, arguments, fixed environment, auxv, random bytes and alignment, retaining at least 4 KiB of headroom. Legacy low-level wrappers terminate on failure. `gw_bootstrap_map_checked` and `gw_bootstrap_stack_checked` return errno values, leave outputs unchanged on failure and reclaim temporary failed mappings. `tests/audit_boundaries.py` checks guard pages, shared sentinels, rejection before stack writes and valid argv/env/auxv.

`rpc_sequence.h` explicitly wraps signed atomic sequences from `INT_MAX` to `INT_MIN`; zero is valid. These are equality tokens for at most one in-flight request per slot, not ordered counters or concurrent producer allocation. Boundary tests exercise real pthread/futex request/response exchanges across both wrap points under four notification/polling configurations.

Core supplies low-level bootstrap, not a universal runtime start/destroy API. Embedders own deployment, stacks, shared memory, signal policy, arguments and confirmation of thread exit. GNU mappings remain until process exit. A timeout or wake does not prove that a peer stopped accessing shared/borrowed addresses; never release its stack or image before exit is confirmed. Process disposal belongs to the embedder's supervisor. Core neither cancels arbitrary functions nor kills the embedder, manages JavaVM, or mixes allocator, FILE and pthread objects between libcs.

See [waiting and diagnostics](docs/en/WAITING.md) and [experimental wake filtering](docs/en/WAKE_FILTER.md). JNI context pairing and object protocols belong to android-jni.

The GNU build verifies patch application even inside ignored directories or a parent Git checkout; see [runtime patching](docs/en/RUNTIME_PATCHES.md).

GNU robust mutex initialization explicitly returns ENOTSUP because kernel robust-list registration is disabled. Ordinary mutexes and condition variables retain their implementation. See the [mutex contract](docs/en/MUTEXES.md) for limits and host-positive-control/selected-runtime tests.
