# GNU mutex contract

[English](../en/MUTEXES.md) | [简体中文](../zh/MUTEXES.md)

This contract applies to the project's GNU runtime. The patch does not affect Host bionic mutexes; pthread objects cannot cross libc ownership boundaries.

## Supported and rejected operations

Ordinary non-robust mutexes retain glibc implementation. Regression tests cover normal lock/unlock, trylock EBUSY, timedlock timeout from another thread, recursive locking, errorcheck EDEADLK and real condition-variable waiting/waking.

`pthread_mutex_init` returns **ENOTSUP** for `PTHREAD_MUTEX_ROBUST`, before writing the mutex object. `pthread_mutexattr_setrobust` may accept the attribute; successful attribute configuration is not successful mutex creation. Do not lock/unlock/destroy an object whose initialization failed. Rejection applies across private/shared, normal/recursive/errorcheck and priority protocols. Tests cover these three mutex kinds, two sharing modes and NONE/INHERIT: 12 combinations. Checking shared attributes does not validate interprocess synchronization.

Libraries requiring robust owner-death recovery must handle ENOTSUP or refuse startup. The bridge neither converts robust locks into ordinary locks nor simulates successful recovery. These tests do not establish complete support for other attributes, PI or interprocess scenarios.

## Rationale

Termux's NPTL patch disables `set_robust_list`. ARM64 glibc still defines `__ASSUME_SET_ROBUST_LIST`, so the userspace fallback on normal pthread exit is not compiled. Removing registration while reporting robust initialization success leaves callers without expected owner-death notification.

With the old artifact in a regular Android app's in-process GNU Guest, all 12 configurations initialized successfully, but after the lock owner returned normally the next timedlock produced ETIMEDOUT rather than EOWNERDEAD. The experiment used bounded waits. The same fixture on standard host glibc verifies EOWNERDEAD, reuse after consistent, and ENOTRECOVERABLE after unlocking without repair as a positive control.

See the [robust mutex manual](https://man7.org/linux/man-pages/man3/pthread_mutexattr_setrobust.3.html). This runtime explicitly rejects robust initialization; compensating for normal exits in userspace cannot replace full kernel registration semantics.

[0003-reject-robust-mutex.patch](../../patches/0003-reject-robust-mutex.patch) checks attribute bits inside glibc initialization, covering its C ABI aliases without adding a symbol shim table. Deploy matching loader/libc together; see [build requirements](RUNTIME_PATCHES.md). This patch does not change JNI private protocol v4.

## Reproduction

From the workspace root, explicitly select installed tools. The ARM64 example requires a GNU cross compiler and QEMU:

```sh
GW_ABI=arm64-v8a core/scripts/build-glibc.sh
python3 core/tests/mutex_contract.py \
  --runtime core/build/glibc-runtime/arm64-v8a \
  --guest-cc aarch64-linux-gnu-gcc --loader ld-linux-aarch64.so.1 \
  --emulator qemu-aarch64
```

The runner first executes the standard-host positive control under ASan/UBSan, then the fixture under the selected loader/libc. Without runtime arguments it runs only the host control. QEMU success does not validate Android or dual-libc execution. `summary.json` records mode, artifact hashes and structured results.

Android links the same C fixture into core's Guest probe. Smoke/coverage require a unique `MUTEX_CONTRACT` JSON confirming 12 robust rejections and ordinary-lock success. jvm-jni owns composition and the APK. After replacing a prebuilt runtime with `GW_DEPS_READY=1`, force preparation/packaging and verify APK hashes; an old APK's PASS is insufficient.

These tests do not cover SIGKILL, arbitrary signal termination, raw SYS_exit, process crashes or the full pthread specification. Explicit rejection avoids claiming robust recovery for those paths.
