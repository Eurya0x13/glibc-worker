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
