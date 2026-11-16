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
The integrator supplies six reviewed, unused public RT numbers. Existing custom
registrations or signalfds prevent enabling with `EBUSY`; the runtime does not
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
`sigsuspend`, `ppoll`, `pselect`, `epoll_pwait` and `epoll_pwait2` retain native
atomic mask installation. Assembly separately retains actual handler-entry
masks and saved return masks. Host handlers see logical `uc_sigmask`; valid
mask edits survive return. NULL mask-wait arguments retain native behavior.

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
remains valid. This protects selection from concurrent registration; it does
not impose native ordering on asynchronous handler dispatch below.

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
records have stable thread-owned storage, not pointers to automatic frames.
A signal detaches a wait record before calling user code. Nested readers,
native `siglongjmp`, GNU `setcontext` and saved-stack resume therefore cannot
leave a dangling consumer pointer or a half-decoded packet. Detached saved
records remain pinned until normal resumption or thread exit; this is not a
general continuation reclamation provider. No initialized jump interposer is
required for this consumption boundary.

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
only its own private notifications and discards its queue. The owner joins
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

Separate fault-injection cases reject query/first-write failures, roll back
partial action/helper restoration, retry successful rollback and verify that
failed rollback pins the router without invoking its reclaim gate. GNU also
denies writable mprotect during stdio restoration to check callback rollback;
Bionic's verified native stdio path needs no table writes.

Other maintained fixtures cover fault recovery, metadata layouts, FD import and
private tables, action generations, one-shots, native masks, pending discard,
consumer cancellation and thread exit. Kernel/policy restrictions are reported
as unavailable. Installed JNI validation additionally checks actual Guest calls,
module close, runtime shutdown and JVM destruction. Android runs use the exact
selected JRE17 in a shell process, not an APK/ART compatibility claim.

Asynchronous native/deferred **handler dispatch priority**, independent Guest
queues/tagged sends, general saved-continuation lifetime and live VM/ART
quiescence remain separate acceptance work. The consuming-order tests do not
prove that the kernel's private RT helper delivery order equals the original
signal handler order. Keep that distinction when enabling the explicit channel.

Upstream: [Linux signal selection](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c),
[native signalfd copying](https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c),
[atomic poll masks](https://github.com/torvalds/linux/blob/v6.12/fs/select.c).
