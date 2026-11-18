# Explicit late signal integration

This experiment supports loading the bridge **after HotSpot startup**, before
application native work begins. It does not rely on late ELF interposition.
Adoption-enabled entrypoints default to same-thread execution when
`GW_DOMAIN_MODE` is absent. Set `GW_DOMAIN_MODE=worker` for an explicit worker
run; empty or unknown values are rejected. Applications using the regular
installed SDK select the backend through the
[runtime startup API](../../../android-jni/docs/en/RUNTIME.md).

## Integration contract

The embedding application must serialize installation against signal
registration, ART chain repair, and application JNI work. This is a bootstrap
operation, not a general attach-to-running-game API. VM/bridge code stays
mapped until process disposal. Shutdown tests destroy the
test HotSpot and then dispose of the process; resuming an arbitrary application
after unloading this bridge is not supported.

There are two explicitly validated paths:

| Host environment | Installed routing |
| --- | --- |
| Standalone bionic HotSpot | Pinned `libjvm` sigaction and dlsym import slots, plus its diagnostic sigaction cache |
| ART and HotSpot in one Android process | Pinned `libsigchain` bottom sigaction/sigaction64 callbacks; VM imports and ART's logical user handlers stay in place |

The standalone path also redirects HotSpot's diagnostic lookup/cache. Patching
only its normal sigaction import leaves the cached lookup bypassing the router.
The original implementation reproduced `handler modified` warnings; the fixed
path keeps that diagnostic enabled and routes its queries correctly.

For ART, the kernel enters the domain router **before** the existing signal
chain. The router restores Host TLS and x18, then calls the saved outer handler,
which can forward through ART to HotSpot. Installation preserves ART's public
SIGSEGV action (handler, flags and mask) and verifies kernel ownership without
reordering the chain. In particular, it must not call `EnsureFrontOfChain` as a
validation step: a pre-existing outer handler that already forwards to ART can
be inserted below ART as well, creating a cycle. The Pojav integration exposed
this failure with repeated `SignalChain::Handler` frames. A special ART user
handler alone is insufficient: ART accesses pthread TLS before invoking those
handlers. Concurrent chain repair remains outside the bootstrap contract.

The signal router preserves kernel flag probing: logical queries and returned
old actions remove request bits rejected by the kernel, including the
`SA_UNSUPPORTED` protocol used by ART. The late contract includes a regression
that failed before this correction.

## Artifact and failure boundaries

- Both paths verify the resident JRE image SHA256 and file identity before
  accessing profile-selected data. Slot alignment, mapping ownership,
  readability, non-executable permissions and original pointer values are
  checked before writing.
- Updates use atomic comparison/exchange and restore the original page
  permissions. They do not patch executable instructions. Rollback refuses to
  overwrite an unexpected third-party update. Irrecoverable rollback errors
  terminate this disposable **experiment**, not a newly added production API.
- The optional ART profile is explicit. The checked-in Vivo Android 13 profile
  only applies to the exact libsigchain digest in that JSON. Another Android
  release/vendor artifact requires separate review; there is no runtime offset
  scanner or automatic fallback to guessed offsets.
- The current late HotSpot profile requires a reviewed diagnostic-cache symbol.
  The stripped desktop JDK used in earlier tests does not supply it, so this
  builder rejects that late configuration. Desktop tests cover the independent
  routing contract, not late HotSpot integration.
- Host-reserved signals still reject Guest registrations with `EBUSY`.
  [Live reclamation](../../../core/docs/en/DOMAINS.md#reclaiming-host-registrations)
  uses the actual native recipients in the application.

The platform implementation used for review is
[Android 13 sigchain](https://android.googlesource.com/platform/art/+/refs/tags/android-13.0.0_r1/sigchainlib/sigchain.cc).
The private HotSpot query path is described in
[OpenJDK 17 signals_posix.cpp](https://github.com/openjdk/jdk17u/blob/master/src/hotspot/os/posix/signals_posix.cpp).
Actual offsets are bound to tested binary digests, not to those moving source
references alone.

## Build and run

Use the already prepared adoption-enabled runtime, NDK and JRE artifacts.
The optional ART profile is for the recorded device artifact only:

```sh
python3 android-jni/experiments/same-thread/build.py \
  --abi arm64-v8a --system-load --late-signal \
  --art-signal-profile jvm-jni/experiments/same-thread/profiles/vivo-android13-sigchain.json \
  --host-libc-signal-profile jvm-jni/experiments/same-thread/profiles/vivo-android13-bionic.json \
  --ndk "$ANDROID_NDK" --jdk "$JDK17" --runtime "$DOMAIN_RUNTIME_ARM64"

python3 jvm-jni/experiments/same-thread/system-load.py \
  --build jvm-jni/build/experiments/late-system-load/arm64-v8a \
  --jdk "$JDK17" --serial "$ANDROID_SERIAL"

python3 jvm-jni/experiments/same-thread/android.py \
  --system-load --inline-vm \
  --native jvm-jni/build/experiments/late-system-load/arm64-v8a \
  --runtime "$DOMAIN_RUNTIME_ARM64" \
  --jre-assets jvm-jni/build/tests/android/assets/android-jre \
  --jre-libs jvm-jni/build/tests/android/jniLibs/arm64-v8a \
  --gradle "$GRADLE" --sdk "$ANDROID_HOME" --serial "$ANDROID_SERIAL"

python3 jvm-jni/experiments/same-thread/late-signal-contract.py
python3 jvm-jni/experiments/same-thread/late-signal-contract.py \
  --ndk "$ANDROID_NDK" --serial "$ANDROID_SERIAL"
```

`--inline-vm` runs ART, HotSpot, bionic and the GNU Guest inside the same test
application process. Without it, the APK runs the native executable in an
application-UID exec child. Both are disposable fixtures; neither modifies
Pojav, accounts, game instances or saves.

The inline run also requires an `ART_SIGNAL_RECLAIM` report. Both public ART
registration entrypoints pass through the installed profile. The app's main
ART thread pauses in the first ordinary-mask callback while the collector
releases its retired mapping. That thread then returns and receives the
replacement.

The shell runner checks both worker and domain transports, then injects a
failure after each standalone binding write to check rollback and VM destruction.
Each successful transport run performs three ClassLoader load/unload cycles;
each cycle adds eight native callers (8,000 calls), a held Guest frame and twenty
stack-walk/GC overlaps while JFR samples native methods. Signal counters
distinguish total suspend/resume deliveries from deliveries that actually
interrupted Guest TLS. Each domain cycle also explicitly raises SR/PIPE/XFSZ
on an adopted carrier and checks Guest TLS and errno after return. Those
synthetic deliveries are reported separately in the pressure rows: JFR sampling
alone did not establish Guest-TLS interruption in the first attempted assertion.
Do not attribute the explicit SR deliveries to JFR. These are compatibility
checks, not performance data.

Final logs, manifests, hashes and result scope are retained under
[results/late-signals](results/late-signals/summary.json).
Passing this fixture is not a claim that the Pojav package or real mods have
been tested with the new backend.

Steady-state performance of this routing path is measured separately in
[PERFORMANCE.md](PERFORMANCE.md), using the same application APK for worker,
same-thread and ordinary bionic JNI paths. Compatibility pressure is disabled
during those timing windows.
