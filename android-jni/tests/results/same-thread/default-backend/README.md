# Default same-thread integration validation

This capture validates the adoption-enabled integration entrypoints after their
missing-backend behavior changed to same-thread execution. It does not migrate
the regular installed SDK, alter archived performance captures, or measure a
new performance result.

Selection rules:

- Absent `GW_DOMAIN_MODE`: same-thread.
- `GW_DOMAIN_MODE=domain`: explicit same-thread.
- `GW_DOMAIN_MODE=worker`: explicit worker.
- Empty or unknown values: reject before admitting JNI calls, without fallback.

Both desktop x86_64 and Android ARM64 runners passed an eight-thread invocation
with the environment variable absent, the explicit worker/domain × 1/8 matrix,
empty/invalid selection rejection, and the two isolated expected FatalError
cases. The default result reports `requested=default`, `backend=domain` and
nonzero adopted-domain creation, not merely an intended configuration.

The desktop System.load runner separately passed three load/unload cycles for
each of absent selection, explicit worker and explicit domain. The Android
application ran ART, HotSpot and both libc domains in one process with no
selection supplied by its supervisor. It passed three load/unload cycles,
ordinary bionic library passthrough, three concurrent signal-pressure rounds,
existing outer-chain preservation and VM destruction. One idle domain stayed
pinned for process disposal according to the existing lifecycle contract.

`summary.json` retains the complete structured outcomes; `manifests.json`
records their as-built source/runtime inputs, and `raw-logs.json.gz` retains the
corresponding stdout/stderr and application report. The APK digest is in the
application result. Hashes and paths identify these captures, not installation
locations or a toolchain lockfile. Older unrelated reports from the reused
ignored output directory are deliberately excluded.

Reproduction uses the build/run commands in [the integration guide](../../../same-thread/README.md)
and [System.load guide](../../../../../jvm-jni/docs/zh/SYSTEM_LOAD.md).
The JNI runner now includes the absent/invalid selection cases before the
explicit matrix. The Android application supervisor removes `GW_DOMAIN_MODE`
for its normal compatibility launch; timed A/B runs still select modes
explicitly. The pinned Vivo Android 13 ART profile and Android JRE17 remain
requirements for the recorded inline application path. See the
[installed SDK startup contract](../../../../docs/en/RUNTIME.md).
