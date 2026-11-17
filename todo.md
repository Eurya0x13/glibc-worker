# Remaining same-thread runtime work

Recovery covers valid native continuations across libc domains and the
bridge-owned resources they abandon. Native transfers that skip Java/VM frames
are outside the project contract; no custom JRE is required to repair them.
Normal JNI returns, Java exception propagation and surviving outer Java frames
remain bridge compatibility obligations. See the
[VM boundary and reproducer](jvm-jni/docs/en/NONLOCAL_VM.md).

Host deferred dequeue/handler ordering and native consumption/lifecycle windows
are implemented and validated within the configured bootstrap contract. The
remaining independent Guest asynchronous delivery and general saved/resumable
native contexts are deferred, not removed.

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

Guest asynchronous signals must be delivered on their target carrier while it
executes Host/Java as well as Guest code. Waiting for the next Guest call is not
an accepted delivery policy. The independent queue substrate is implemented in
core; see [Guest signal inbox](core/docs/en/GUEST_SIGNAL_INBOX.md). Its native
notification test is not acceptance of foreign TLS, JNI or VM signal entry.
Guest handler execution must also remain transparent to a suspended Host
blocking wait. Internal notifications do not create Host `EINTR`; preserve its
original timeout and still expose real Host-owned signal interruptions.
Active bootstrap atomic mask-waits, nullable masks, `poll`, `select` and `epoll_wait` now
have hidden-notification retry with one monotonic timeout budget. Public LP64
sleep APIs now retain native timers, copyout, errno and cancellation while
retrying hidden work; see the
[sleep adapter](core/docs/en/SIGNAL_WAITING.md#transparent-native-sleeps).
This is a Host-side prerequisite, not a production Guest entry pass. Ordinary
blocking I/O and private/raw paths still need review.
The native-only sleep control now reproduces remainder-pointer `EFAULT`, errno
conventions and differing hrtimer/ppoll saturation on both kernels. A sleep
adapter retains those semantics rather than reusing a ppoll remainder;
see [sleep controls](core/docs/en/SIGNAL_WAITING.md#native-sleep-controls).

The [native cross-libc entry](core/docs/en/GUEST_ASYNC_ENTRY.md) now connects a
Guest-owned inbox to actual adopted TCBs and guarded stacks. It validates busy
Host/Guest interruption and transparent Host sleep/ppoll with real Guest work.
The normally returning fixture is not public Guest sigaction/send integration,
asynchronous nonlocal recovery or a JNI/VM compatibility pass.

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

- Complete independent asynchronous dispositions/masks, pending queues, default
  actions and targets; keep overlapping registrations from replacing VM handlers.
  Thread/shared queue selection, aggregate RT accounting and notification/close
  transactions are implemented; connect the inbox to actual Guest recipient
  admission, disposition generations, tagged libc send/wait hooks and timely
  entry during Host execution. Retain kernel restart/EINTR semantics when the
  logical action changes or a notification has no deliverable Guest packet.
  [Private NPTL isolation](core/docs/en/PRIVATE_SIGNALS.md) separates GNU's
  cancellation/timer and credential channels from Bionic's private signals;
  Bionic bootstrap routing and reviewed late data-import routing now cover
  real private registrations/entry. GNU Host cancellation and credential
  protocols still require their own lifecycle validation.
- Implement tagged Guest `raise`, `pthread_kill`, `sigqueue` and supported aliases,
  preserving sender information, queue semantics and errors.
  The optional [owning-libc send hook](core/docs/en/GUEST_SIGNAL_SEND.md) now
  connects these public sites, `gsignal`, `pthread_sigqueue` and same-process
  `kill` to the cross-libc fixture. Exit-lock races, synchronous self-delivery
  and errno checks are included; production recipient/disposition integration
  remains unfinished.
  The [core target registry](core/docs/en/SIGNAL_TARGETS.md) now supplies dynamic
  lookup, shared eligibility and pinned retirement in that cross-libc fixture;
  installed JNI service wiring and saved-frame integration remain required.
  Optional [NPTL creation callbacks](core/docs/en/THREAD_LIFECYCLE.md) now
  reserve identity before clone and complete admission before creation returns;
  production recipient wiring must use them rather than child entry alone.
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
