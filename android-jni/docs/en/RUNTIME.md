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

Start returns zero or an errno value. Before activation, the service and Host
negotiate their private transport version, session layout and required
capabilities, and the service verifies the patched libc's adoption ABI.
An ordinary or incompatible libc is rejected. A failure after bridge
initialization begins quarantines the runtime and retains its resources; the
caller must discard the process rather than retry initialization.
Validation failures that leave the bridge in `GW_HOST_NEW` permit a corrected
retry. The runtime claims the bridge before creating a dispatcher or enabling
Guest fault routing, so a rejected configuration cannot leave either side
effect behind. Terminal failures after that claim do not roll back helpers.

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
Before acknowledging STOP, the supervisor joins its workers, blocks every
catchable signal through the native kernel mask and revokes its Host signal
registry entry. It then parks permanently in a native futex and never polls
application commands again. Host checks the acknowledged mask before stopping
its dispatcher. This prevents an idle GNU supervisor from entering a native
Host handler after router restoration. `SIGKILL`/`SIGSTOP` retain native kernel
semantics; this is not complete VM/ART recipient quiescence or permission to
unmap the GNU runtime. The diagnostic thread name is `gw-guest-owner`.
The private transport requires the matching quiescent-STOP capability;
an older service is rejected before activation.
The bridge's [lifecycle contract](LIFECYCLE.md) applies to both backends.

`build-guest.sh` builds the matching service, and `scripts/package-runtime.sh`
stages it with the adoption-enabled libc and a `domain-runtime.json` manifest.
`GW_DOMAIN_RUNTIME` can select an explicitly built matching runtime directory.
The package script rejects a manifest with the wrong ABI or adoption version.
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

The ownership classifier is implemented, but full independent Guest signal
semantics are still pending; [core domains](../../../core/docs/en/DOMAINS.md) lists the
current supported boundary. Default same-thread selection does not remove that
boundary.

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

Tagged sends, dispositions and blocked-set consumption use the Guest libc ABI.
Logical registration and sending for job-control signals return `EBUSY`.
Guest signalfd and the remaining I/O and saved-context acceptance cases are in
[the compatibility work list](../../../todo.md).
