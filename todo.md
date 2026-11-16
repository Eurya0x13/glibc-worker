# Remaining same-thread runtime work

Recovery covers valid native continuations across libc domains and the
bridge-owned resources they abandon. Native transfers that skip Java/VM frames
are outside the project contract; no custom JRE is required to repair them.
Normal JNI returns, Java exception propagation and surviving outer Java frames
remain bridge compatibility obligations. See the
[VM boundary and reproducer](jvm-jni/docs/en/NONLOCAL_VM.md).

Current implementation order: descriptor lifecycle/concurrent Host delivery
and Host delivery on GNU-created threads first. Independent Guest asynchronous
delivery and general saved/resumable native contexts are deferred, not removed.

Host handlers received on genuine GNU pthreads use the explicitly accepted
[Host dispatcher policy](core/docs/en/HOST_SIGNAL_RELAY.md). Native
[Host stdio backends](core/docs/en/STDIO.md) cover the opt-in deferred signalfd
channel while retaining real FILE objects and the verified Bionic native path.

## Agreed signal ownership

Use explicit ownership. Synchronous hardware faults belong to the executing
domain, before applying asynchronous reservations. VM-reserved asynchronous
signals belong to Host; tagged Guest sends to those signals fail with `EBUSY`.
Other runtime-tagged Guest signals belong to Guest. Untagged external signals
belong to Host. Raw syscalls bypassing the runtime cannot identify a Guest
logical target.

The ordinary service enforces Host signal reservations. The opt-in unwind
service additionally supports independent synchronous Guest dispositions,
adopted-carrier logical masks, native get/set/swapcontext mask adaptation and
kernel retention of Host-reserved pending signals. Adopted wait interfaces now
protect those queues across temporary masks, consuming waits, pending queries
and signalfd creation/update; see [signal waiting](core/docs/en/SIGNAL_WAITING.md).
Positive JNI recovery and
default-fatal-action cases run on desktop and Android ARM64. These mechanisms
are not full signal virtualization. The supported interface is documented in
[core domains](core/docs/en/DOMAINS.md) and [JNI runtime](android-jni/docs/en/RUNTIME.md).
Bootstrap Host registration helpers have native-libc comparison coverage;
nine Guest aliases have installed JNI recovery coverage on both platforms.

## Implement independent Host and Guest signal delivery

- Complete production integration of the deferred fault channel. The opt-in
  `--host-fault-mask` fixture recovers Guest traps with Host blocked and checks
  deferred/coalesced siginfo, logical pending queries and three Host consuming
  waits. Live context retirement refuses pending work; thread exit discards only
  its own notifications. Host atomic mask-waits retain their native handler and
  return masks, including Bionic LP64 aliases. Host registrations now route all
  public signals without expanding VM ownership; unadopted pre-existing custom
  actions fail Guest admission. Host signalfd read/readv, duplicate aliases,
  metadata and six readiness interfaces are implemented. Kernel-authoritative
  descriptor inspection now covers imported/duplicate aliases, range close and
  private tables; lifecycle scans include every live task's table. Complete
  Host `setcontext`/resumable read-decode and close windows and paths
  without initialized native-jump interposition. Managed consuming operations
  now retire consumed packets before outside native jumps or cancellation;
  nested readers and fresh handler sends retain separate ownership. Complete
  logical dequeue ordering across native/deferred queues, remaining
  kernel/helper selection races and lifecycle
  windows before
  enabling this channel in ordinary integrations. Passing this fixture is not
  general independent asynchronous delivery.
  Successful `SIG_IGN` registrations now invalidate older pending originals even
  after a handler is restored, while preserving already consumed packets; this
  does not finish the remaining disposition-selection races. Deferred selection
  now serializes against registration and consumes a one-shot before exposing
  the handler mask; native/deferred replacement and nested-default tests cover
  these boundaries. Helper entries now bind the kernel-selected Host generation
  and `SA_RESTART`; controlled native/deferred blocked-read comparisons check
  restart and `EINTR`. Initial recipient publication reconciles private masks.
- Complete independent asynchronous dispositions/masks, pending queues, default
  actions and targets; keep overlapping registrations from replacing VM handlers.
- Implement tagged Guest `raise`, `pthread_kill`, `sigqueue` and supported aliases,
  preserving sender information, queue semantics and errors.
- Keep the explicit GNU-to-Host relay contract covered while extending signal
  integration. Relayed handlers intentionally execute on a different, real Host
  pthread; source frames remain alive until they return. No fictitious JVM
  identity or original-thread Host TLS adoption is implied.
- Complete general saved-continuation lifetime accounting beyond the tested
  owned signal stacks and AUTODISARM suspend/resume paths. Independent native
  alternate stacks are implemented in the opt-in integration. Extend synchronous
  fault coverage to FPE, all registration aliases and normal/blocked
  initialization/teardown windows. Retain nested delivery, reset-handler and
  nonlocal-exit regressions, and cover VM suspension/profiling protocols.
- Implement transparent cross-domain nonlocal recovery in the first release,
  including `siglongjmp` and `setcontext`. Same-domain-only support or quarantine
  on an otherwise valid cross-domain jump does not satisfy this requirement.
  Complete the restoration and lifecycle acceptance matrix in
  [nonlocal recovery](core/docs/en/NONLOCAL_RECOVERY.md); a successful jump alone
  is not a compatibility pass.
  The opt-in installed service now coordinates Guest-target jumps through a
  reviewed C-only `GetVersion` boundary, with invocation-generation records,
  ordinary RPC cleanup and the owning glibc's fortified restore entry. Complete
  other native JNI callback boundaries and general saved/resumable native
  contexts without skipping Java/VM frames; this slice does not establish
  general Guest-target recovery.
- Supply a reviewed live VM/ART recipient-quiescence provider for the explicit
  `signal_router_reclaim` protocol. Its standalone single-recipient gate is
  implemented/tested; a JVM safepoint alone is insufficient. Without a provider,
  production registration entries remain pinned.
- Cover initialization/teardown windows, remaining mask/wait APIs and registration
  aliases. The implemented wait guard does not supply logical Guest queues or
  virtualize a Host descriptor passed across domains.

## Complete installed/runtime integration validation

- Validate explicitly selected late HotSpot/ART profiles through the installed
  jvm-jni target; unconfigured profiles must continue to fail with `ENOTSUP`.
- Revalidate Pojav's own exact JRE/profile, ordered setup, matching runtime and
  mod lifecycle before replacing its premain bridge. No arbitrary live attachment
  or bridge unload while application threads resume is implied.

Delete this file only after these items are implemented and verified. Do not
relabel reservations or isolated routing tests as full dual-domain compatibility.
