# Same-thread domain experiment

## Current signal ownership contract

The first version reserves VM-required signals for the Host. Guest libc must
reject conflicting registration with `EBUSY`, including `signal()` aliases.
Queries for reserved dispositions also fail with `EBUSY`: a Host function
pointer is not a callable Guest handler. Reservations cannot be removed during
the runtime lifetime. Direct signal syscalls and bypassing the patched libc are
outside this experiment's supported contract.

## Deferred: independent Host and Guest signal semantics

Implement only after production Guest JNI compatibility is demonstrated,
performance measurements justify adopting same-thread execution, and the project
chooses that execution backend. This is not a prerequisite for the restricted
first experiment and must not be reported as implemented.

- Virtualize dispositions, signal masks, default actions and signal targets.
- Define delivery to Host carriers, adopted Guest domains and Guest-created
  pthreads, including process-directed signals and pending signals at switches.
- Support overlapping registrations without allowing Guest code to replace VM
  handlers; define ordering and consumption rules explicitly.
- Cover synchronous faults, nested delivery, alternate stacks, reset handlers,
  nonlocal exits and VM suspension/profiling protocols.
- Replace experimental process-lifetime disposition retention with a reviewed
  publication and reclamation protocol. Concurrent replacement now binds an
  immutable entry to each generation and has a 1,000-replacement contract test;
  general reclamation remains unimplemented.
- Cover initialization/teardown windows and all supported registration APIs.

Until these are implemented, rejecting a conflicting Guest module is an expected
compatibility result, not evidence that full signal virtualization works.

## Required before Pojav same-thread integration

Explicit late signal integration now exists in the isolated experiment; see
[jvm-jni LATE_SIGNALS.md](jvm-jni/experiments/same-thread/LATE_SIGNALS.md).
Its ART path requires a separately reviewed, exact libsigchain artifact profile.
Do not replace a Pojav premain bridge until its own JRE/profile, bootstrap
registration ordering, matching runtime payload and mod lifecycle have been
validated. The test APK is not a Pojav/game compatibility result.

Keep full signal virtualization and process-lifetime record reclamation deferred
as above. Do not infer support for arbitrary live attachment, unreviewed Android
ART artifacts or unloading the bridge while the application resumes execution.

## Regular SDK migration of the same-thread backend

The adoption-enabled integration build now defaults to same-thread execution;
`GW_DOMAIN_MODE=worker` remains an explicit compatibility selection. This does
not by itself migrate the installed SDK. Its CMake targets currently compile
the worker Host, and its package script starts `service_main.c` with the normal
runtime. Simply enabling the experiment macro would leave undefined hooks or
pair the new service with a libc that lacks the adoption entrypoints.

Promotion must preserve the four project boundaries and provide these pieces:

- Move architecture switching, thread adoption, signal routing and the patched
  runtime recipe into maintained core sources and installed build targets.
  Keep JNI and JRE dependencies out of core and bionic-dlopen.
- Define an explicit public Host bootstrap/configuration API for runtime image
  paths, selected VM, backend and signal ownership. The current shared session
  and `start_owner` are private integration structures, not an installed ABI.
- Separate generic JNI transport initialization from pinned HotSpot/ART late
  signal setup. `late-signal.c` currently verifies artifacts through JvmAdapter;
  android-jni must not acquire a dependency on the upper jvm-jni project.
- Package the matching adoption-enabled Guest service and libc together, with
  a protocol/capability check before admitting calls. `package-runtime.sh`
  currently selects the ordinary runtime and worker service.
- Validate the independently installed CMake packages, default/worker selection,
  generic Host lifecycle and explicit artifact rejection. Keep JRE/ART profiles
  maintainer-selected; no version/offset autodetection is implied.

A default change in the independent integration entrypoints is covered by its
JNI and System.load runners. It is not evidence that these public SDK/build
contracts have already been implemented.
