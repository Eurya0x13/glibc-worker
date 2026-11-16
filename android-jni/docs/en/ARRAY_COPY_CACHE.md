# Primitive array copy cache

[English](../en/ARRAY_COPY_CACHE.md) | [简体中文](../zh/ARRAY_COPY_CACHE.md)

`src/host/array_copy.c` provides a bounded Host copy cache for JNI Get/ReleasePrimitiveTypeArrayElements. It neither returns raw JVM heap pointers nor changes TLS/RPC protocols.

## Selection

Arrays totaling 1 MiB through 8 MiB, inclusive, qualify. At most eight slots and 32 MiB of data capacity are allocated on demand; this excludes malloc metadata, JVM heap and fallback VM copies. Smaller/larger arrays, slot/budget exhaustion and allocation failure use the real JVM Elements path.

Selection is automatic by default. `GW_HOST_DISABLE_ARRAY_COPY_CACHE` disables it and takes precedence over the retained legacy enable bit `GW_HOST_ARRAY_COPY_CACHE`. Public configuration structures and Guest protocol are unchanged.

Buffers remain actively allocated. Android allocators may retain virtual addresses after free while returning physical pages; rewriting later temporary copies causes minor faults. Reusing live allocations avoids repeated free/page-touch without changing process allocator policy or locking physical pages. First use still allocates/touches memory and JNI content copying remains.

One MiB is 1,048,576 bytes. Thresholds use bytes, not element count: 262,144 int elements reach the lower bound. Slot and byte limits are independent; small qualifying arrays occupying all slots can force fallback.

## Ownership and release

Host allocates with its own allocator, fills via real GetArrayRegion and reports isCopy=JNI_TRUE. Return buffers only through matching Release; Guest must not free them.

| Mode | Behavior |
| --- | --- |
| 0 | Copy back and return slot |
| JNI_ABORT | Return slot without copy-back |
| JNI_COMMIT | Copy back while retaining the same copy; final release remains required |

All eight primitive types are covered. Active pointer/type identify ownership, supporting valid cross-call/cross-thread release. The cache holds no JNIEnv, ClassLoader or JNI object references; callers keep array references valid. No VM call occurs under the cache lock.

Copy-back preserves an existing pending exception's identity. Shutdown frees idle capacity after Host helpers quiesce; outstanding active copies are retained with diagnostics rather than freed into dangling pointers. Normal exit requires `active=0 retained_bytes=0`.

## Use and validation

Set the disable bit in gw_host_bridge_config.flags. Test-app `--array-copy-cache auto|on|off` options are fixture inputs, not production runtime flags.

`tests/native/array_copy_checks.c` and `tests/java/benchmark/PayloadModule.java` run under jvm-jni's real VM host: eight types, COMMIT lifetime, release modes, exception identity, GC, concurrent borrows, cross-thread return and capacity fallback. See the [integration manual](../../../jvm-jni/tests/README.md).

The cache reduces repeated allocation/page-touch, not actual copying. Thresholds do not guarantee equal benefit on every VM/workload. Pin JRE, APK, cache state and workload; compare latency, faults, retained capacity and full save/exit. Microbenchmark percentages are not game/mod gains.

From the workspace root, `python3 android-jni/tests/array_cache.py` checks bounds, COMMIT lifetime, exceptions, 12-borrow slot fallback, cross-thread return, 32 MiB budget and cleanup under ASan/UBSan. `tests/native/allocator_pages.c` independently probes Android allocator page return: it temporarily changes its own M_DECAY_TIME, compares repeated allocations with retained buffers, and uses mincore for residency. Never link it into products or change app allocator policy through it.

## Source mechanisms

Android 13 Scudo [`MapAllocatorCache::store`](https://android.googlesource.com/platform/external/scudo/+/8e2d762272cd7bed1362cbe22016d1d9754d74f8/standalone/secondary.h) can return physical pages while retaining mappings in immediate-release mode; ineligible allocations are unmapped. [`M_DECAY_TIME`](https://android.googlesource.com/platform/external/scudo/+/8e2d762272cd7bed1362cbe22016d1d9754d74f8/standalone/wrappers_c.inc) selects an allowed release interval. These explain possible mechanisms, not uniform behavior across Android vendors/processes/sizes.

[OpenJDK17 JNI Elements](https://github.com/openjdk/jdk17u/blob/jdk-17.0.14-ga/src/hotspot/share/prims/jni.cpp#L2235) allocates/fills primitive-array copies and frees on final release. Retaining live allocations reduces repeated faults. Virtual address reuse does not prove physical residency; total array time is not bridge scheduling cost.
