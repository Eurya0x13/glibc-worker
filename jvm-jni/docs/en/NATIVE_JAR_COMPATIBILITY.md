# Unmodified GNU JNI JAR compatibility

[English](../en/NATIVE_JAR_COMPATIBILITY.md) | [简体中文](../zh/NATIVE_JAR_COMPATIBILITY.md)

## Goals and pinned inputs

Exercise JAR native-resource selection, System.load, GNU DT_NEEDED closure, real data and resource lifecycle. Use upstream unmodified JAR/ELF artifacts without requiring author recompilation. Pure Java mods do not count as GNU compatibility coverage.

| Artifact | Pinned version | Main coverage |
| --- | --- | --- |
| zstd-jni | 1.5.7-6 | Compression/decompression, errors, streams, native threads, two load lifecycles |
| lz4-java | 1.8.0 | Arrays, DirectBuffer, streams, concurrency |
| snappy-java | 1.1.10.12 | Native codec, streams, checksums, bad input |
| rocksdbjni | 10.2.1 | Database I/O, column families, direct buffers, concurrency, reopen |

JRE/zstd hashes are in tests/config/targets.json; extra JAR/source-JAR/per-ABI native hashes in tests/config/native-jars.json. Licenses belong to tests/third_party/ and download caches to this project's vendor/.

## Dependencies and deployment

`tests/scripts/prepare-native-jars.py` checks ELF architecture, SONAME, DT_NEEDED and hashes. GNU math/rt/dl use core's matched runtime. GNU libgcc/libstdc++ are discovered through the selected compiler and must match tests/config/native-jars-runtime.json; mismatches stop rather than trusting arbitrary local libraries.

Android deploys lib*.so images. Guest preloads dependencies by absolute installed paths then resolves original SONAMEs. JARs still extract their own resources and call System.load; content policy finds identical installed bytes. No assume-loaded/native-path bypass or symbol whitelist is used.

## Running

From the workspace root:

```sh
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar lz4
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar snappy --reuse-apk
jvm-jni/tests/coverage/run.sh --device SERIAL --native-jar rocksdb --reuse-apk
```

The first build enables GW_NATIVE_JARS=1; --native-jar configures it. Each library uses a new app test process, first completing basic JNI/SQLite/zstd coverage, then its data/lifecycle checks. Do not concurrently contend for the same test package.

## Pass criteria and limits

Require regular app UID, correct JRE/ABI, unmodified hashes, all functional assertions, two resource lifecycles, logical ClassLoader unload, reference cleanup, Guest stop and JVM destruction. Timeout, CheckJNI violation, wrong data or incomplete exit fails. tests/check.py enforces results under build/test-results/.

Logical unload does not guarantee mappings disappear; GNU UNIQUE and upstream behavior can retain DSOs. Closing RocksDB need not immediately stop process-wide background pools. Record resource ownership, threads and mappings separately rather than equating them with leaks or complete unload.

Library success does not validate an entire Minecraft mod. Follow with individual-mod function/save/exit, then pinned multi-mod workloads. Investigate performance only on reproducible real paths.
