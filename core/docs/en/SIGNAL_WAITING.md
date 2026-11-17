# Signal masks and waiting

[English](../en/SIGNAL_WAITING.md) | [简体中文](../zh/SIGNAL_WAITING.md)

This contract applies to active adopted Guest calls in the explicitly enabled
native-unwind service, using the matching libc. It protects Host-reserved
signals; complete asynchronous Guest delivery remains unfinished.

## Interface behavior

The adopted Guest has a logical mask; its suspended Host retains its own mask.
For a Host-reserved signal, the physical kernel mask uses the Host bit. Guest
changes cannot unmask a Host-blocked signal or suppress an unblocked VM signal.
Other bits follow the existing Guest mask adapter. Raw syscalls bypass it.

| Interface | Adaptation |
| --- | --- |
| `sigsuspend`, `ppoll`, `pselect`, `epoll_pwait`, `epoll_pwait2` | Translate the temporary execution mask, retaining the suspended Host's reserved bits. |
| `sigtimedwait`, `sigwaitinfo`, `sigwait` | Exclude Host-reserved signals from the consumable set. The latter two retain native delegation to `sigtimedwait`. |
| `sigpending` | Hide Host-reserved signals from the Guest query without changing kernel queues. |
| `signalfd` | Exclude Host-reserved signals when creating or updating a Guest descriptor's mask. |

A consumable set is not an execution mask. Copying Host mask bits into it would
authorize consuming Host signals. A reserved-only set becomes empty; timed waits
retain native timeout/error behavior. No signal is dequeued and reconstructed:
Host realtime values, sender information and FIFO order stay in the kernel.

An existing Host signalfd passed across domains retains its Host-created mask.
The adapter does not take ownership of arbitrary descriptors. Callers still
define descriptor-sharing rules.

## Mask transaction

`wait-mask.inc` saves physical/logical masks with catchable signals blocked,
publishes the temporary Guest logical mask, and constructs the physical syscall
argument. The owning libc's existing syscall atomically installs it and waits.
Ordinary and error returns restore both original masks and preserve result/errno.
Updating only the syscall argument would leave Guest handlers observing the
previous logical mask.

TLS retains no pointer to the automatic transaction record, so a nonlocal exit
cannot leave a pointer to that abandoned frame. Native capture/restore adapters
still determine the selected jump/context API's mask semantics. A Host-target
jump uses bridge unwind and does not execute the abandoned wrapper's epilogue.

NULL temporary masks and unbound GNU pthreads retain native paths, including
cancellation. The patch retains glibc's cancellation wrappers and timeout code.
Reviewed paths are LP64 time64 on x86_64/AArch64; time32 is not established.

## Host deferred channel

Link `glibc_worker::signal_interpose`, initialize/adopt the router, then call
`gw_host_signal_channel_enable(notifications, GW_HOST_FAULT_SIGNAL_COUNT)` from
`signal_channel.h` before starting the VM or creating Host signalfds/recipients.
The integrator supplies six reviewed, unused public RT numbers. Occupied notification
numbers or existing Host signalfds prevent enabling with `EBUSY`; the runtime does not
choose numbers or silently replace handlers. The JNI SDK `--host-fault-mask`
case consumes this installed public API with a matching native-unwind service.
Ordinary SDK startup does not enable it automatically.

The channel lets Guest execute with fault numbers unblocked while Host retains
its logical mask. Original asynchronous fault-number packets belong to one
carrier recipient; standard packets coalesce and retain the first siginfo.
Private notifications carry no user pointers. Public registrations of those
six numbers return `EBUSY`. This is not independent Guest asynchronous delivery.

Host mask queries, consuming waits and descriptor APIs translate these private
bits. Bionic resolves bottom LP64 entries to avoid alias recursion. Host
`sigsuspend`, `ppoll`, `pselect`, `epoll_pwait` and `epoll_pwait2` use atomic
LP64 kernel mask installation when the channel is active. A stable row and an
identifiable syscall PC/SP keep the temporary execution mask separate from
the saved return mask, including nested frames selected before user entry.
GNU cancellation is checked while setup remains delivery-blocked and after
normal retirement; no automatic cleanup record survives into a user handler.
Assembly separately retains actual handler-entry masks and saved return masks. Host handlers see logical `uc_sigmask`; valid
mask edits survive return. Nullable mask-waits atomically snapshot the current
physical mask when no temporary mask is supplied; they do not apply input-mask
filtering to this snapshot. `poll` and `epoll_wait` use those guarded paths too.
With the channel inactive all these interfaces retain their native backend.
Input filtering follows each owning-libc interface: Bionic's mask/poll/select
paths filter its private signals, while epoll passes its temporary mask through.
A mask query makes no temporary mask mutation, and BLOCK/UNBLOCK apply input
filters before combining with existing bits; a native action mask can already
have private libc bits blocked.

## Transparent Host wait recovery

Active bootstrap `sigsuspend`, `ppoll`, `pselect`, `epoll_pwait` and
`epoll_pwait2`, including Bionic LP64 aliases and nullable masks, suppress
`EINTR` caused only by an empty/ignored internal notification. `poll`,
`epoll_wait` and `select` share the guarded implementation. A real Host handler
keeps its native interruption, including during hidden work or between
retry attempts. A successful transparent retry preserves the caller's errno.

A hidden wait stays published until the retry decision. Actual Host handler
entry marks published hidden ancestors visible, including across nested waits.
Ordinary visible deliveries keep the existing syscall-PC/SP return-mask binding.
Pending cancellation is checked before another atomic syscall with delivery
blocked; the bounded cancellation cleanup retires the row and restores its mask.
Blocking deferred-consuming waits also retry hidden-only notifications regardless
of the caller's ordinary `SA_RESTART` policy.

One monotonic timeout budget covers the entire operation, including time spent
in hidden work. Remaining time is recomputed rather than reusing the kernel's
pre-handler remainder. Saturation is unnecessary: elapsed time is subtracted
from the original valid duration, avoiding an overflowing absolute deadline.
Expired waits make a final zero-time readiness check. Millisecond epoll waits
round their remaining duration upward; negative millisecond timeouts remain
infinite. Invalid native timeout inputs still reach the kernel unchanged.
See Linux [`ppoll` and timeout handling](https://github.com/torvalds/linux/blob/v6.12/fs/select.c).

`select` additionally returns the final syscall's remaining timeout through its
mutable `timeval`, including after visible interruption. Its input conversion
follows the carrier's LP64 libc: GNU normalizes its microsecond field with
overflow saturation, while Bionic rejects out-of-range microseconds. Kernel
fd-set handling remains native; Linux leaves input sets intact on `EINTR`, so
a hidden retry observes the same requested descriptors. Pipe-based fixtures
verify readiness and output sets/events after retry for all six basic wait APIs.

The regression fixture injects empty notifications before atomic entry, delays
notification work past the deadline, enters a nested wait, and delivers real
Host signals both during hidden work and after retry publication. A separate
native-only control demonstrates that `SA_RESTART` alone still exposes `EINTR`.
The fixture exercises notification recovery under Host TLS; it is not acceptance
of a production Guest handler entered while Java executes.

This is not general Host syscall virtualization. Ordinary blocking I/O,
private libc calls and raw syscalls do not acquire this
recovery scope. General saved/resumable native continuations and actual Guest
asynchronous entry still require the remaining integration/lifetime work.

### Native sleep controls

`signal_sleep_control` links only the native thread library and reports
`CONTROL_ONLY`. It establishes sleep-adapter requirements independently of the
opt-in implementation. Both Linux and Android ARM64 show these differences:

- `nanosleep` and relative `clock_nanosleep` ignore an inaccessible remainder
  pointer on successful completion, but return `EFAULT` when interrupted and
  required to copy the remainder. Pre-validating the output or writing it with
  an unchecked C assignment changes this behavior.
- Relative hrtimer sleeps saturate their expiration at the kernel's signed
  nanosecond horizon. The raw `ppoll` syscall reports a different remainder for
  the same `INT64_MAX`-second input. Reusing its timeout output for a sleep
  remainder would therefore change valid-input semantics.
- `clock_nanosleep` returns an error number while preserving errno;
  `nanosleep` returns `-1` and sets errno. `SA_RESTART` does not suppress the
  observed interruption in either interface.

The sender repeats native thread signals until the wait returns, avoiding a
fixed delay that assumes syscall entry. The inaccessible output is a real
`PROT_NONE` mapping. No Host/Guest switching is involved. See Linux
[`do_nanosleep`, `nanosleep_copyout` and timer initialization](https://github.com/torvalds/linux/blob/v6.12/kernel/time/hrtimer.c)
and [`poll_select_finish`](https://github.com/torvalds/linux/blob/v6.12/fs/select.c).

### Transparent native sleeps

With the bootstrap channel enabled, public LP64 `nanosleep`, `clock_nanosleep`,
`usleep` and `sleep` now retry hidden-only notifications. They retain the native
`clock_nanosleep` syscall rather than substituting `ppoll`. Relative sleeps
count hidden work against one deadline and the kernel's actual saturated
remainder. Absolute sleeps retry the original clock/deadline, retaining its
clock adjustment rules; they never access the remainder pointer. Actual Host
handlers retain native interruption. A visible handler before syscall entry
does not shorten an unstarted sleep, even if a hidden notification immediately
follows it. `clock_nanosleep` preserves errno and returns the error number;
the other entry points retain the carrier's LP64 conventions.

An identifiable assembly unmask/entry window closes the early-notification
race. A helper selected there skips the unstarted syscall and recomputes the
budget instead of returning to an indefinite sleep. Return-mask capture retains
valid Host handler edits. Only stable process-owned rows are published.

Remainder copyout occurs only after a visible relative interruption. The kernel
validates it through a bootstrap-created, populated, private memfd, preserving
`EFAULT` and partial-copy behavior without an unchecked C store. The serialized
copy transaction blocks catchable signals and performs no per-signal allocation.
Its one descriptor/page remain pinned with the process runtime, even after
router restoration. Application code must not close/replace that descriptor;
an absent or mismatched descriptor returns `ENOTSUP`. Bootstrap fails if the
required memfd cannot be created. This is not general pointer validation.

GNU deferred cancellation is enabled across the native syscall window and
restored before user handlers or hidden work. Compiler forced-unwind cleanup
retires the wait and restores its mask. It publishes no automatic pthread
cleanup-chain pointer that a native signal jump could abandon. Bionic has no
`pthread_cancel` or public get/setcontext API; the Android fixture reports those
cases as unavailable. See
glibc's [cancellation signal entry](https://github.com/bminor/glibc/blob/glibc-2.41/nptl/pthread_cancel.c)
and [cancel-type publication](https://github.com/bminor/glibc/blob/glibc-2.41/nptl/pthread_setcanceltype.c).

`signal_sleep_test` runs the native control contracts plus hidden work,
nested/visible interruptions and relative/absolute clock cases.
`signal_sleep_lifecycle_test` observes actual blocked syscalls through
`/proc/self/task/TID/syscall`, then tests notification, cancellation, native
`siglongjmp`/`setcontext`, subsequent sleeps and recipient teardown. It also
queues a helper in the pre-entry rearm window. These are Host-side acceptance
tests. GNU additionally suspends a live sleep handler onto a separate native
context, performs another sleep there, and resumes the original handler twice
before allowing its interrupted sleep to retire. The fixture owns both stacks
until all saved work has returned; it does not reclaim arbitrary saved contexts.
The suites run on Linux x86_64 and Android ARM64 and are not proof of independent
Guest handler entry during Java execution or general saved-continuation
reclamation.

## Linux consuming order and atomic commit

Host `sigtimedwait`, `sigwaitinfo`, `sigwait`, `read`, `readv` and native FILE
read backends use the same logical selection rules. Eligible thread pending
precedes process-shared pending. Within a queue, Linux prioritizes the six
synchronous-fault numbers even for asynchronous packets, then chooses the
lowest number; other signals use numeric priority and RT packets retain FIFO.
An already assigned deferred packet is thread pending. This does not reverse
the kernel's earlier choice of recipient for a process-directed arrival.

When a deferred packet participates, selection includes its winning private
level and all native numbers with higher or equal logical priority. The kernel
still selects newly arriving higher-priority native signals. A native copy of
the same standard thread signal coalesces with the older deferred packet;
a same-number process-shared copy remains separate. Sends after dequeue are
fresh instances and are not drained as duplicates. The native signalfd supplies
actual native record layouts, including positive fault codes; userspace only
encodes its own deferred asynchronous metadata.

Kernel selection, original-packet consumption and output conversion run in one
bounded transaction, with catchable signals blocked and cancellation disabled.
The registration writer serializes selection with `SIG_IGN` and disposition
changes. No user handler executes under that writer. `sigpending` also takes
one coherent native/logical snapshot under it. An ignore generation discards
older pending work even after a handler is restored, while committed output
remains valid. Kernel selection and logical commit share the registration
transaction; asynchronous handler dispatch uses the original-number model below.

Each deferred recipient owns one nonblocking native signalfd, initially with
an empty mask. It is separate from application descriptors and closed when the
recipient retires. A short-lived private wake lets the kernel validate/copy
output before conversion, retaining native `EFAULT` consumption and the running
kernel's vector-layout rules. Original iovec metadata and one-record prefixes
are validated before logical dequeue. Zero iovecs retain native zero returns.
Application code must not close, replace or import runtime-owned descriptors.
Ordinary descriptors and calls with the channel disabled retain native paths.

The owning libc's zero-length read/wait entry preserves cancellation policy
without consuming data. GNU FILE no-cancel reads keep their native no-cancel
entry. Blocking occurs outside the transaction through an atomic `ppoll` wait;
`SA_RESTART`, `EINTR`, return masks, deadline and errno are preserved. Wait
records use a process-owned stable pool with O(1) normal recycling, not pointers
to automatic frames. Visible signals detach their matching wait row before user
code; hidden-only notifications retain it through the retry decision. Nested readers,
native `siglongjmp`, GNU `setcontext` and saved-stack resume therefore cannot
leave a dangling consumer pointer or a half-decoded packet. Detached saved
records and referenced parents stay pinned with the process runtime; ordinary
unreferenced returns recycle their rows. Pages remain process-owned, including
after thread exit. This conservative policy is not a general continuation
reclamation provider. No initialized jump interposer is
required for this consumption boundary.

## Linux asynchronous handler selection

Before a Host mask exposes deferred fault numbers, the recipient moves their
kernel level from a private notification to the original number. Shadow metadata
remains until kernel selection binds it: a newer native same-number thread send
coalesces with that earlier packet; a post-dequeue send remains a fresh instance.
Both original fault numbers and ordinary native signals therefore participate
in the kernel's own priority, thread/shared selection and restart decision.
Guest return remaps the suspended Host mask before restoring delivery.

The six original fault routes and private entries have a full outer kernel
mask. They bind the selected immutable disposition and shadow packet before
exposing the logical user action mask. Ordinary signal routes retain their
native kernel masks. User masks, `SA_NODEFER`, saved return masks and valid
ucontext edits remain observable through the public adapters. Installing
`SIG_IGN` flushes a promoted original level in the kernel and invalidates its
shadow metadata; a selected old frame keeps its old disposition/metadata, while
fresh sends after restoration belong to the new generation. Retirement also
synchronizes completed ignore generations before deciding `EBUSY`.

This boundary requires configured bootstrap mask/wait interposition. Raw syscalls
and private libc atomic mask-waits bypass its stable records and cannot be
claimed to preserve temporary-mask reconstruction. It does not establish live
VM/ART signal-protocol compatibility or independent asynchronous Guest queues.

## Descriptor and lifecycle ownership

Host `signalfd`/Bionic `signalfd64`, `read`/`__read_chk`, `readv`, native
[stdio backends](STDIO.md), duplicate aliases and `fcntl` duplicate commands
operate on actual kernel OFDs. Duplicate/imported descriptors share native masks
and status flags; each read uses its own recipient. Modern kernels allow split
records across iovecs; the tested Android 5.4 kernel rejects that layout before
consuming with `EINVAL`. Ordinary kernel records pass through unchanged.

Active-channel reads inspect `/proc/thread-self/fd` and `fdinfo`, including
private FD tables. There is no numeric-FD cache or descriptor lease to retire.
Owners serialize application descriptor replacement/mode changes against reads.
Proc inspection uses bounded raw syscalls and closes temporary descriptors
before restoring delivery. Inaccessible inspection returns `ENOTSUP` without
consuming data. `poll`, `select`, `epoll_wait` and mask-waits prune only cancelled
levels before checking readiness. Raw/private libc paths outside the configured
bootstrap/stdio adapter bypass this contract.

`close` keeps the native cancellation point without a metadata transaction,
writer lock or hidden duplicate. Nonlocal exits cannot retire a reused number.
Bootstrap/restore scans the actual tables of all live tasks, including private
`files_struct` instances. A descriptor retaining a private notification mask
prevents router restoration. Empty masks do not retain notification ownership.

Live recipient retirement refuses pending work with `EBUSY`. Thread exit drains
only its owned private/promoted kernel levels and discards its queue. The owner joins
recipients, destroys the VM, closes/clears notification-bearing descriptors and
quiesces saved continuations before restoring the router on the enabling thread.
Other Host mask changes survive that restore.

Restoration snapshots native actions, changes only necessary entries and rolls
back all completed writes on recoverable failure. A clean rollback leaves the
router active and permits retry. Failed rollback, partial Guest fault setup or
an unrecoverable stdio mutation returns terminal `EIO` on subsequent operations:
retain code/state, reject admission and reclamation, and let the owner dispose
of the process. The runtime does not automatically terminate Host.

## Validation and remaining boundary

```sh
python3 core/tests/signal_deferred.py
python3 core/tests/signal_deferred.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

The ordering fixture compares six consuming interfaces against raw Linux
selection for mixed priority, thread/shared priority, standard coalescing,
shared duplicates and RT FIFO. Controlled arrivals enter after candidate
construction; fresh post-dequeue sends, bad output pointers and native vector
validation have separate assertions. Blocking read/readv checks observe an
actual `ppoll`, then check restart, EINTR, nested reads and outside native jumps.
GNU-only checks cover context abandonment, a saved read stack resumed after an
independent read, blocking `setcontext` and close/context transfer. Bionic lacks
public get/set/swapcontext and pthread cancellation APIs; unavailable cases are
reported explicitly and do not count as Android coverage.

The handler fixture compares actual native/routed handler entry order,
execution/return masks and payloads for public atomic mask-waits, full/empty/
asymmetric action masks, thread/shared queues, coalescing and selected action
generations. Registration before channel bootstrap, same-number post-dequeue
sends, ignore/restore windows and nested waits have separate comparisons.
GNU-only cases cover cancellation before/after the final masked setup check,
blocked cancellation and native saved-wait context abandonment/resumption on
retained stacks, without linking the jump interposer.

Separate fault-injection cases reject query/first-write failures, roll back
partial action/helper restoration, retry successful rollback and verify that
failed rollback pins the router without invoking its reclaim gate. Bootstrap
query/write/rollback failures also cover widening existing fault routes. GNU also
denies writable mprotect during stdio restoration to check callback rollback;
Bionic's verified native stdio path needs no table writes.

Other maintained fixtures cover fault recovery, metadata layouts, FD import and
private tables, action generations, one-shots, native masks, pending discard,
consumer cancellation and thread exit. Kernel/policy restrictions are reported
as unavailable. Installed JNI validation additionally checks actual Guest calls,
module close, runtime shutdown and JVM destruction. A separate installed JNI
process injects terminal router damage after a successful call, then checks
that subsequent calls, image close and shutdown refuse further execution and
retain resources. The test owner disposes of that deliberately pinned process.
Android runs use the exact
selected JRE17 in a shell process, not an APK/ART compatibility claim.

Independent Guest queues/tagged sends, general saved-continuation lifetime and
live VM/ART quiescence remain separate acceptance work. Native Host handler
selection is covered only within the explicitly configured bootstrap contract.

Upstream: [Linux signal selection](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c),
[native signalfd copying](https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c),
[atomic poll masks](https://github.com/torvalds/linux/blob/v6.12/fs/select.c).

## Consuming logical Guest signals

The owning GNU libc provides the optional version 1 consuming hook through
`__gw_domain_signal_consume_hook`. Installed consumers compile
`signal-consuming.c` and `signal-user-copy.c` with Guest libc. They bind the
hook to a live logical recipient and enable wait notification before admitting
targets.

`sigpending` exposes the recipient's blocked private and shared pending bits.
`sigwait`, `sigwaitinfo` and `sigtimedwait` consume the requested blocked set
using private pending before shared pending, then Linux ordering within that
queue. GNU's public wait API normalizes `SI_TKILL` to `SI_USER`. Kernel
copyout retains `EFAULT` and a packet selected before an invalid output is
consumed. An invalid input or timeout fails before selection.

The registry samples pending work and its futex generation together. Accepted
publication, recipient close and handler entry advance the generation. A
publisher can therefore arrive between the last sample and the native wait
without losing a wake. The owning GNU libc performs the cancellation point.
A user Guest handler can interrupt an unmatched wait, while an unrelated
kernel delivery leaves it waiting against the same monotonic deadline.
Selection that finds a matching packet wins over a concurrent interruption.

The GNU-only control through `core/tests/guest_async.py` checks public wait
entrypoints, pointer faults, real kernel pending isolation, blocking producer
wake, cancellation and visible Guest interruption. The installed JNI worker
can opt in to the same consuming hook. Guest signalfd and the adopted
carrier's consuming integration remain separate work.
