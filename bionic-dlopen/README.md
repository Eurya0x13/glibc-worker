# glibc-worker bionic-dlopen

[English](docs/en/README.md) | [简体中文](docs/zh/README.md)

A JNI-independent Host loading layer: GNU ELF classification, image routing, generation-tagged logical handles, duplicate-inode rejection, borrow counts and close synchronization. Implementation lives in `src/`; public loading contracts live in `include/`.

## Standalone build

Install core first, then run from this project. Only core's installed package is consumed, not its source or tests:

```sh
./build.sh -DCMAKE_PREFIX_PATH=/path/to/core/install \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
ctest --test-dir build --output-on-failure
```

Outputs are `build/libgw_dlopen.so`, `build/libgw_dlopen.a` and the `GlibcWorkerDlopen` package exporting `glibc_worker::dlopen`. Java, JNI headers and libffi are not discovered.

## Interface

See [dlopen_backend.h](include/dlopen_backend.h) and the [C loading contract](docs/en/LOADER.md). Configure the C transport, image resolver and optional observer, activate, then use the versioned function table.

The call context is `void *`; pure C clients may pass NULL. Metadata is an adapter-defined int32 result. Binding descriptions, JNI descriptors and JNI_OnLoad version validation are interpreted above this layer.

GNU symbols are not executed directly in the bionic domain, and no ELF loader is reimplemented. The transport performs GNU dlopen/call/close in the correct Guest domain; its adapter guarantees returned entrypoint ABI and lifetime. This is not process-wide dlopen interception.

AUTO returns `GW_PASS` only after successful ELF parsing finds no GNU marker. Unreadable, malformed or unsupported images return `GW_ERROR/GW_E_IMAGE`; classifier resource limits return `GW_ERROR/GW_E_CAPACITY`. Neither failure may retry through the platform loader. Explicit `GW_OPEN_GUEST` requires caller-confirmed GNU ABI and skips classification.

Classifier bounds are 128 program headers, 512 dynamic entries, 256-byte names including terminators, 64 version-dependency groups and 256 versions per group. Exceeding them is a failure, not a non-glibc result. Classification does not replace relocation, symbol resolution or full validation by the real loader.

## Tests

`tests/loader_contract.c` uses a C transport to check NULL contexts, non-JNI metadata, lookup/calls, duplicate images, rollback, stale handles, busy close and stop restrictions. It proves the Host contract, not independent dual-libc startup.

`execution_scopes.c` checks ancestor/recursive scopes, thread ownership and cross-thread close; `close_waits.c` checks two/three-thread close cycles and unborrowed targets; `loader_quarantine.c` covers four quarantine cases. See [execution scopes](docs/en/EXECUTION_SCOPES.md) for execution borrows versus transferable lifetime pins.

`tests/elf_route.py` checks real Android/GNU ELF, exact SONAME matching, unmarked images, malformed metadata and resource limits under ASan/UBSan. It can consume upper-layer test DSOs. C tests also ensure AUTO errors neither return PASS nor call Guest transport. jvm-jni owns real dual-libc lifecycle integration.

The backend is configured once per process. STOPPED and quarantine cannot be reconfigured. Stop checks/stops the loading backend; it does not cancel calls or destroy GNU runtime. Embedders must stop producers, return borrows and close modules. Close waits and transport execution have no universal deadline. Watchdog quarantine does not prove published calls stopped accessing resources; stacks, buffers and images must remain until safe. The embedder owns process disposal.

## Borrow-cost diagnostics

`tests/scope_cost.c` compares legacy pins and execution-scope bookkeeping through real exports/mutexes with a test transport; it executes no JNI, Guest or RPC. The runner requires explicit old/candidate libraries, NDK, device and an empty output directory. It uses 1/8 threads, warmup and ten balanced pairs, retaining samples, temperature and input hashes:

```sh
python3 bionic-dlopen/tests/scope_cost.py \
  --device "$ANDROID_SERIAL" --android-ndk "$ANDROID_NDK" \
  --baseline-library /path/to/baseline/libgw_dlopen.so \
  --candidate-library /path/to/candidate/libgw_dlopen.so \
  --output bionic-dlopen/build/scope-cost-review
```

Wall/CPU values are batch-amortized enter/leave costs, not JNI latency or game gains. Both libraries must be actual builds supporting their selected exports; filenames do not establish implementation versions.
