# GNU runtime patch-build contract

[English](../en/RUNTIME_PATCHES.md) | [简体中文](../zh/RUNTIME_PATCHES.md)

Use GNU loader/libc artifacts from the same build. The `scripts/build-glibc.sh` cache key includes the build script, patch applicator, target configuration and patch contents. Changes select new source/object directories.

Verification happens when this glibc build script runs. Workspace `GW_DEPS_READY=1` directly consumes existing artifacts; callers must already have confirmed a verified matching build.

`scripts/apply-patches.py` resolves the source's real path, clears inherited Git worktree/index variables and sets the Git discovery ceiling at the source parent. An ignored source directory inside a parent checkout is thereby treated independently of the caller's prefix.

A successful `git apply` exit code is insufficient: running in a repository subdirectory with an absolute `--directory` can skip every path yet return zero. Verify reverse-patch applicability after each application before writing the patched marker, and verify again when reusing the cache. Failure stops compilation/publication immediately.

Missing patches affect Android compatibility. Without NPTL changes, loader `__tls_init_tp` calls `set_robust_list` (ARM64 syscall 99), potentially triggering SIGSYS under app seccomp. That is a Guest startup failure, not a JNI capacity check.

```sh
python3 core/tests/runtime_patches.py
```

Tests cover ignored and standalone source layouts, polluted `GIT_DIR/GIT_WORK_TREE`, forward application, reverse verification, repeated-application rejection and corrupt-cache rejection. They modify only temporary core build directories and need no glibc download/rebuild.

Correct patching does not remove compatibility tradeoffs. With robust-list registration disabled, `0003-reject-robust-mutex.patch` returns ENOTSUP on robust initialization while retaining ordinary locks. See [mutex limits and tests](MUTEXES.md).
