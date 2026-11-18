# Runtime bootstrap API

[English](../en/RUNTIME.md) | [简体中文](../zh/RUNTIME.md)

The installed `runtime_bridge.h` provides `gw_host_runtime_start` and
`gw_host_runtime_shutdown`. It uses the installed core domain target and the
normal JNI bridge; no experiment header or test executable is required.

The start configuration is versioned and explicit:

- `backend`: zero selects same-thread; `GW_EXECUTION_WORKER` selects worker.
- `loader_path`, `library_path`, `service_path`: the matching packaged GNU
  runtime and service, deployed by the embedder.
- `bridge`: the existing image policy, JNI bridge flags and optional registered
  VM adapter.
- `signals`: the immutable ownership contract matching the already prepared
  Host signal router.

The worker backend is in maintenance mode. Embedders can select
`GW_EXECUTION_WORKER` explicitly for compatibility and diagnostic comparisons.
Worker continues to receive correctness and compatibility fixes and retains SDK
regression coverage. New execution features and performance work target the
default same-thread backend.

The selected VM comes from the supplied real `JNIEnv`. android-jni neither
creates a VM nor chooses an ART/HotSpot profile. An embedder prepares the signal
chain before admitting application calls; any pinned VM-specific late routing
is supplied by jvm-jni. It must not pass an environment from another VM.

Start checks `signal_router_guest_ready()` before bridge initialization. A
pre-existing custom Host action that has not been explicitly adopted returns
`ENOTSUP`. Executable bootstrap consumers can initialize/adopt the router before
creating the VM; late consumers use their reviewed integration. Host registration
routing does not make unreserved signal numbers exclusive to the VM.

Use a dedicated bootstrap/lifecycle thread. Its context zero serves the Guest
supervisor's control channel. Application native calls run on other attached VM
threads, which receive same-thread adopted domains or worker bindings according
to the configured backend. This preserves the existing owner/control model.

Start returns zero or an errno value. Before launching GNU code, Host opens
the service, checks its private transport version in an ELF note and passes
the same open file to the loader through `/proc/self/fd`. The supervisor
starts with catchable signals blocked. It verifies the shared layout,
required capabilities and patched libc ABI before registering its Host-facing
signal entry and applying the inherited mask. A failure after bridge
initialization begins quarantines the runtime and retains its resources; the
caller must discard the process rather than retry initialization.
Validation failures that leave the bridge in `GW_HOST_NEW` permit a corrected
retry. The runtime claims the bridge before creating a dispatcher or enabling
Guest fault routing, so a rejected configuration cannot leave either side
effect behind. Overlapping start and shutdown calls return `EBUSY` before
touching the transport. Terminal failures after the bridge claim retain helpers.

Before starting GNU code, startup creates the real Host dispatcher described by
the [signal relay contract](../../../core/docs/en/HOST_SIGNAL_RELAY.md). A routed
Host arrival on a genuine GNU pthread waits for that dispatcher to finish its
handler. Both backends preserve this explicit change of handler thread; the
dispatcher is not implicitly attached to the VM and must not invoke JNI from a
signal handler. If the embedder already started the dispatcher, startup borrows
it and shutdown leaves it running. Otherwise startup owns it and shutdown joins
it after Guest control stops. Pending relay work refuses shutdown in either
case. The external owner joins a borrowed dispatcher after runtime shutdown and
before restoring the router. External senders must already be quiescent.

Shutdown requires the lifecycle owner, stopped producers, quiesced native work
and closed modules. It does not cancel native calls, destroy the VM or unload
saved stacks/TLS/code. Idle VM bindings and the GNU supervisor remain pinned.
Open Guest signal descriptors and active Host descriptor waits return `EBUSY`
before bridge shutdown commits. Closing the last descriptor reference allows
another shutdown attempt. After a successful shutdown, another shutdown call
returns `EALREADY`.
Before acknowledging STOP, the supervisor joins its workers, blocks every
catchable signal through the native kernel mask and revokes its Host signal
registry entry. It then parks permanently in a native futex and never polls
application commands again. Host checks the acknowledged mask before stopping
its dispatcher. This prevents an idle GNU supervisor from entering a native
Host handler after router restoration. `SIGKILL`/`SIGSTOP` retain native kernel
semantics; this is not complete VM/ART recipient quiescence or permission to
unmap the GNU runtime. The diagnostic thread name is `gw-guest-owner`.
The readiness, descriptor preflight and STOP-acknowledgement waits each use a
15-second monotonic deadline. After bridge shutdown commits, a failed STOP or
owned relay join
returns terminal `EIO` with its remaining resources pinned.
The matching private transport supports descriptor preflight and the
quiescent STOP acknowledgement. An older service is rejected before activation.
The bridge's [lifecycle contract](LIFECYCLE.md) applies to both backends.

`build-guest.sh` builds the matching service, and `scripts/package-runtime.sh`
stages it with the adoption-enabled libc and a `domain-runtime.json` manifest.
`GW_DOMAIN_RUNTIME` can select an explicitly built matching runtime directory.
The package script rejects a manifest with the wrong ABI or adoption version.

### Packaged glibc library names

The package validates the architecture and exact `DT_SONAME` of each bundled
glibc image. The `glibc_aliases` object in `domain-runtime.json` records the
ABI names and their packaged files. For example, `libdl.so.2` maps to
`libguest_dl.so` on both ABIs. Use `compat/glibc-libs` in `library_path` for an
unpacked compatibility package. Keep its relative symlinks when relocating
the package.

For an APK, deploy the matching `libguest_*.so` files as installed native
libraries and keep their bytes unchanged during packaging. Include the
ABI-matched `glibc-libs.tsv` as an asset. Before starting the runtime, call
`GuestLibraries.prepare(privateStorage, nativeLibraryDir, catalogStream)` from
the Host. The helper checks the installed files against the asset and creates
reusable links in app-private storage. Use the returned directory in
`library_path` and use the installed loader and service for `loader_path` and
`service_path`. The latter paths can still use the packaged filenames.

GNU lookup resolves bare names, including `DT_NEEDED`, through the search
path. For a path-based open, use an existing alias entry, such as
`<alias-directory>/libdl.so.2`. The built-in aliases use the complete ABI
names. App-supplied dependencies use the normal GNU search rules in the
directories and RUNPATHs supplied by the embedder or the dependency.

Run the regression against the packaged loader and helper JAR. Add
`--runner qemu-aarch64` for an arm64-v8a package on an x86_64 machine with the
GNU cross compiler installed.

```sh
python3 android-jni/tests/runtime-aliases.py --package "$GW_PACKAGE" \
  --helpers "$GW_PACKAGE/glibc-bridge-helpers.jar"
```

The low-level `full_host_start` API remains available for embedders that supply
their own worker bootstrap; it does not silently create a runtime.

`tests/runtime_sdk.py` builds a consumer using only installed CMake packages and
headers. It checks default/worker selection, JNI load/callback/array/TLS paths,
lifecycle and rejection of an ordinary libc. All dependency paths are explicit
command-line arguments. Shutdown tests inspect the supervisor's actual kernel
signal mask and futex wait through `/proc/self/task`, in addition to the API
result. No production runtime path depends on these `/proc` observations.
Its build logs, manifest and structured results live in
`android-jni/build/tests/runtime-sdk/`. `tests/runtime_sdk_android.py` repeats the
installed-package checks on ARM64 with an explicitly supplied reviewed JRE
startup fixture/profile; this test does not move VM creation into android-jni.
Android results live in `android-jni/build/tests/runtime-sdk-android/`.

The ordinary service enforces Host signal reservations. Independent Guest
delivery across libc domains requires the explicit configuration below.
See [core signal ownership](../../../core/docs/en/DOMAINS.md).

The opt-in unwind service uses owning-Guest [action snapshots](../../../core/docs/en/SIGNAL_ACTIONS.md)
for synchronous faults. Re-registering a handler no longer allocates a pinned
record. An in-flight delivery retains its own complete action copy.

With the opt-in native unwind build, a V2 config can set `guest_notification`
to an explicitly unused public RT number. An occupied number fails admission.
The matching Guest service reserves real pthread
identities before clone, publishes their TID before returning from create,
and routes tagged packets to the owning GNU signal entry. Ordinary Host
packets still use the Host dispatcher. Cancellation and credential handlers
for GNU channels 63/64 keep their native registration. STOP checks that all
application recipients have retired before releasing the Guest registry.

The same-thread backend also requires the deferred Host signal channel and
native unwind interposition to be ready at bootstrap. Its Guest recipient
uses the actual adopted GNU TCB while the carrier executes Java, waits in
Host libc or runs a routed Host handler. Nested entry uses the most recent
Guest frame and records the live stack boundary before the switch. See
[Guest asynchronous entry](../../../core/docs/en/GUEST_ASYNC_ENTRY.md).

Tagged sends, dispositions and blocked-set consumption use the matching Guest
libc ABI. Guest signalfd supports readiness, descriptor aliases and concurrent
close. Job-control sends keep native Linux behavior, with shared Host-owned
dispositions. A Guest attempt to change a job-control disposition returns
`EBUSY`. The signal waiting contract covers Host timeout and restart behavior,
including the native limits of private and direct syscalls. See
[signal waiting](../../../core/docs/en/SIGNAL_WAITING.md).

Supported native transfers restore saved contexts and reconcile abandoned
bridge-owned JNI resources. The contract requires valid native targets and
preserves surviving Java frames. See
[nonlocal recovery](../../../core/docs/en/NONLOCAL_RECOVERY.md) and
[installed SDK tests](SDK_TESTS.md).
