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

## Validation and limits

The internal `signal_router_defer_faults` trial separates reserved fault
execution bits from Host's mask. Only the explicit JNI `--host-fault-mask`
fixture enables it. Six unused RT signals become runtime-owned; registrations
return `EBUSY`. Original siginfo stays in a lazily allocated recipient queue.
Host mask/query/consuming-wait interposition translates notification bits and
retains native reserved-signal filtering and cancellation entry. Bionic resolves
bottom `sigprocmask64`/`sigtimedwait64` entries to avoid public-alias recursion.
Host `sigsuspend`, `ppoll`, `pselect`, `epoll_pwait` and `epoll_pwait2` translate
their temporary masks, including Bionic LP64 aliases. NULL masks retain the
native path. Assembly preserves the actual kernel handler-entry mask separately
from `ucontext.uc_sigmask`: Linux mask-waits save the pre-wait mask there for
return. Routed Host handlers see logical context masks; valid edits are converted
back on return.

The explicit Host bootstrap target also adapts `signalfd`/Android `signalfd64`,
`read`/`__read_chk`, `readv`, `dup`, `dup2`, `dup3` and duplicate commands of
`fcntl`/`fcntl64`. A kernel signalfd includes notification lanes independently
of its creator: consumption uses the reading thread's recipient. Ordinary
kernel records remain byte-for-byte unchanged; notification records become
128-byte `signalfd_siginfo` records with the original code, PID, UID and union
fields. Vector reads retain the running kernel's layout rules: modern kernels
allow splitting a record across iovecs, while the tested Android 5.4 kernel
returns `EINVAL` before consuming it. Native reads retain
blocking, nonblocking, partial-read and cancellation behavior; no hidden FD is
pinned. Duplicate descriptors share the kernel OFD mask and status flags.

`poll`, `select`, `epoll_wait` and the mask-wait interfaces prune this recipient's
cancelled notification levels before checking readiness. They never discard a
live original signal. While this explicit channel is active, public reads
inspect `/proc/thread-self/fd` and the kernel signalfd mask in `fdinfo`.
`thread-self` is required for private FD tables. There is no numeric-FD cache
or metadata lease to retire after close/replacement. Duplication, SCM_RIGHTS
imports, raw close and `close_range` therefore retain the actual OFD semantics.
Owners must still serialize replacement against active reads. Inspection uses
raw, non-cancelling syscalls with catchable signals blocked, allocates no memory
and closes its temporary proc descriptors before restoring delivery. Native
[Host stdio backends](STDIO.md) translate before bytes enter FILE buffers;
raw reads/private libc handles outside those backends bypass the public adapter.
Logical dequeue ordering across native signals and deferred notification lanes
also remains unfinished; the mixed-record test checks metadata, not equivalence
of signal-selection priority. A native metadata comparison is not that proof.
Raw syscalls/private libc handles bypass public interposition.

When the public native-jump interposer is linked and `gw_nonlocal_init()` has
completed, managed reads and consuming waits also own their syscall-to-decoder
window. Signal entry records which private notifications the kernel has already
consumed before invoking a Host handler. These originals are no longer logically
pending. A handler's fresh send has separate ownership, including when the old
notification was a cancelled level. The selected notification handler retains
ownership of its own delivery; nested readers do not steal an outer result.
Normal return decodes the staged packet. An outside `siglongjmp` retires it on
the ordinary Host stack; pthread cancellation uses the owning libc's cleanup
entry. Consumed native output is discarded, without discarding fresh sends.
An active consumer prevents live context retirement with `EBUSY`.

The production invocation still enters native libc read/wait cancellation
points. Copy failures retain native dequeue behavior, and a zero-iovec read
retains native zero-return behavior. Tests inject a signal after the native
operation and before decoding, including nested readers, handler-local jumps,
outside jumps, cancelled notifications and a selected notification handler.
Desktop additionally checks cancellation after consumption; Bionic has no
public pthread cancellation API. This does not cover Host `setcontext` or
resumable saved contexts. Without initialized jump interposition the adapter
uses its ordinary decode path; full window recovery is not established there.

`close` retains its native cancellation point without a metadata transaction,
writer lock, cancellation suppression or a hidden duplicate. A jump/cancellation
cannot strand a numeric-FD record or erase a replacement. Lifecycle inventory
checks the actual tables of all live tasks, including private `files_struct`
instances; thread exit and range close require no userspace retirement hook.
Bootstrap/restore still require descriptor owners to be quiescent. Inaccessible
proc inspection returns `ENOTSUP`, rather than guessing that a table is empty.
The native-close-window and cancellation regressions remain in the suite.
This does not establish arbitrary `setcontext` exits from a native consumer.

Successful Host `SIG_IGN` registrations advance an immutable discard generation.
The recipient applies it before exposing or consuming its logical pending queue;
restoring a handler does not revive older pending originals. Registration never
walks foreign TLS or another thread's possibly retiring queue. Already consumed
packets retain their ownership: a missing armed notification belongs to the
reader/selected handler, rather than to the pending queue being discarded.
New sends during ignore are discarded. A 64-round barrier-controlled fixture
alternates native and deferred pending inputs while another thread installs
ignore and restores the previous action. It also checks fresh sends afterward
and read/wait results consumed before the ignore change. This does not establish
full equivalence of concurrent helper selection, Host registration and one-shot
action consumption; those disposition races remain integration work.
Public mask changes, native jump restoration and Guest-to-Host command returns
prune invalidated levels before unblocking. The fixture also checks unmasking
and a saved-mask jump without first querying pending state or reading a record.
Real Host synchronous faults with a logical `SIG_IGN` retain native fatal
semantics. The outer route must not turn an ignored hardware fault into an
endless retry. Separate native/routed child processes must both terminate by
`SIGSEGV`, rather than by the fixture timeout. Independent Guest trap recovery
while Host ignores that number is checked by the installed JNI fixture.

A native close can free its number before its wrapper returns. Kernel object
inspection prevents an unrelated replacement's notification-like bytes from
being decoded. An inspection failure returns `ENOTSUP` without consuming data.
This adds proc syscalls only while the explicit deferred channel is active;
it is a correctness change, not a performance improvement claim.

Enabling the channel refuses any existing Host signalfd, including untracked
ones, with `EBUSY`. Restoration refuses an OFD whose actual mask still includes
a private notification, including imported descriptors and another thread's
private table. An empty mask does not retain notification ownership. Scans run
only at serialized lifecycle boundaries, not as ordinary read-side registry
walks. A five-case fixture covers raw duplicates, SCM_RIGHTS, range close,
private-table retention and an untracked pre-enable descriptor. Native kernel
or policy restrictions are reported as `UNAVAILABLE`, not positive coverage;
private-table tests fall back to `unshare(CLONE_FILES)` when `close_range` is
absent.

Deferred-handler selection, its immutable Host action and one-shot consumption
are selected under the registration writer transaction. The transaction ends
before application code runs. A routed one-shot is consumed before the handler
mask is exposed, so nested delivery sees the default action. Barrier-controlled
native/deferred tests replace the selected action, including an intervening
ignore, before entering the user handler; the selected generation survives.
Each helper kernel entry also binds the immutable Host generation and its
`SA_RESTART` flag. A helper selected before a concurrent ignore/replacement
keeps that generation; a new generation cannot revive a packet invalidated by
ignore. A four-case control observes a real blocked pipe read, then delivers
native/deferred packets with and without restart. Deferred packets are published
by a controlled fixture producer under the selection lock; this is not a new
public cross-thread queue API. The original positive restart case returned
`EINTR` before the fix. Failure after a transient first installation rolls back
the kernel action and pins the possibly selected record until quiescence.
Native/deferred dequeue-priority equivalence is still unfinished. New recipient
publication also reapplies its logical mask, so initial private blocking cannot
strand its first signal.

Live context retirement returns `EBUSY` before freeing Guest state if work is
pending. Consumption/unblocking permits retry. Thread exit instead drains only
its private notifications and discards its pending work. Router restoration
refuses active deferred recipients. Once they have retired, the enabling thread
restores the router and its original private-notification mask bits; other Host
mask changes survive. A later router lifetime cannot reuse that private mask.
This does not prove saved-continuation quiescence.

Run `python3 core/tests/signal_deferred.py` from the workspace root, adding
`--ndk /path/to/ndk --serial DEVICE` for Android ARM64. It checks real faults,
coalesced metadata, kernel waiting/signalfd notifications, discard/fresh sends,
notification-send retry and recipient retirement. A native-libc comparison checks
temporary/NULL/blocked wait masks, saved context mask edits, immutable timeouts
and saved/unsaved jumps from a handler. Desktop exercises five wait interfaces;
Android exercises eight public/LP64 entries and skips two unavailable
`epoll_pwait2` entries with `ENOSYS`. Host descriptor coverage compares complete
records against raw kernel signalfd for five siginfo layouts, tests mixed
ordinary/deferred records, native vector layouts, alias mask updates, FD replacement,
six readiness interfaces, cancellation tombstones and another reader thread.
Four blocking cases compare ordinary/vector reads against raw kernel-created
descriptors for `EINTR`, observing the actual blocked syscall before sending the
interrupt. Four further desktop cases cover pthread cancellation. Bionic exposes
no `pthread_cancel` API; Android reports it unavailable, not passed. Recipient
cleanup must complete on every exercised path.
The low-level queue fixture still explicitly decodes its raw private wake.
Concurrent disposition changes and the descriptor integration gaps above remain
unfinished; this trial must not be enabled as general asynchronous delivery.

The [SDK fixture](../../../android-jni/docs/en/SDK_TESTS.md) checks 19 wait cases
and two Host-target jumps from an observed kernel `rt_sigsuspend` wait. It checks
another JNI call, explicit fixture cleanup, module close and shutdown. An absent
`epoll_pwait2` is reported as skipped. The desktop environment exercises it;
the tested Android ARM64 JRE17 shell environment returns `ENOSYS` and skips two
cases. Its error-return mask restoration is checked, but its successful waiting
paths need a supporting kernel. This does not validate an APK sandbox or ART.

Logical Guest queues, tagged sends, Host delivery on GNU pthreads, general
saved-continuation lifetime and untested aliases/windows remain required. These
guards protect existing Host queues without claiming those remaining mechanisms.

Upstream mechanisms:
[pending selection/suspend](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c),
[poll/select mask installation](https://github.com/torvalds/linux/blob/v6.12/fs/select.c),
[signalfd consumption](https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c).
The runtime manifest binds actual source and checked patch inputs.
