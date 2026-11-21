# Private libc signal channels

[English](../en/PRIVATE_SIGNALS.md) | [简体中文](../zh/PRIVATE_SIGNALS.md)

The matching adoption libc ABI 7 uses kernel signal 63 for GNU cancellation
and its `SIGEV_THREAD` timer helper, and 64 for the NPTL credential protocol.
Its dynamic public RT range is 34 through 62. Modules use their owning libc's
`SIGRTMIN`/`SIGRTMAX`; equal offsets in different libcs do not identify equal
kernel channels. Hardcoded GNU implementation-private signal numbers are not
a module ABI.

The JRE Host also reserves kernel 62 for its networking interruption protocol;
it is independent of HotSpot's USR2 suspend/resume signal. See
[libnet](https://github.com/openjdk/jdk17u/blob/master/src/java.base/linux/native/libnet/linux_close.c)
and [libnio](https://github.com/openjdk/jdk17u/blob/master/src/java.base/unix/native/libnio/ch/NativeThread.c).
Guest tagged sends to that Host reservation return `EBUSY`.

Upstream GNU NPTL uses 32/33, which overlap Android's timer and backtrace
channels. Starting GNU's first pthread otherwise replaces a process-wide Host
disposition before any module installs a handler. See the owning
[GNU definitions](https://sourceware.org/git/?p=glibc.git;a=blob;f=sysdeps/unix/sysv/linux/internal-signals.h)
and the [Android 13 definitions](https://android.googlesource.com/platform/bionic/+/android13-release/libc/platform/bionic/reserved_signals.h).
The runtime builder patches these definitions, retains GNU's native handlers,
forced unwind and timer helper, and includes the patch in its recipe hash.

## Bootstrap and lifetime

Call `signal_router_lease_guest_nptl()` during serialized bootstrap, before
starting the matching GNU loader. The installed JNI runtime does so in its
owner startup. Both channels must have their original `SIG_DFL` action, no
router registration and no other reservation. A conflict returns `EBUSY`
without changing either channel. An enabled, healthy router is required.
Successful repetition is idempotent; it does not create another runtime.

The lease is permanent for the process. Host public action registration and
queries return `EBUSY`, including after logical router shutdown. Verification
and restoration leave these GNU-owned dispositions alone. GNU's root thread,
timer helpers and images remain process-pinned; logical bridge shutdown does
not establish native thread exit. Applications must exclude these channels
from Host signal sends and raw registrations. A returned or destroyed bridge
does not give the channels back to Host.

GNU private handlers enter directly under GNU TLS, retaining cancellation's
owning unwind implementation. Borrowed carriers still reject pthread
cancellation and credential broadcasts; moving signal numbers does not turn
them into kernel-created GNU threads. No foreign libc function is called from
the native GNU private handler.

On Android, the Host reservation mask also contains its actual private range,
32 through `SIGRTMIN - 1`. This lets the adopted Guest mask adapter preserve
the carrier's Host mask. Guest public dispositions cannot replace those
handlers. Reserving private mask bits does not make them public logical
packets in `gw_signal_contract_owner`.

## Validation and limits

`signal_private` checks conflict rejection, no partial lease, native Guest
writes, verification, restore and permanent Host rejection. The real
[cross-libc fixture](GUEST_ASYNC_ENTRY.md) snapshots raw kernel actions 32..40,
starts and cancels a genuine GNU pthread with a cleanup handler, and fires a
GNU `SIGEV_THREAD` timer callback. It requires unchanged Host private actions,
installed GNU private actions and native GNU TLS in cleanup/callback code.

The bootstrap interposer includes Bionic's private range in Host routing and
forwards pre-`main` registrations natively before enabling the router. Its real
`android_run_on_all_threads` registration/send path is exercised while adopted
carriers execute GNU code. The native Bionic handlers run with restored Host
TLS. Their ordinary Guest syscall interruptions retain native EINTR semantics.

Installed JNI tests exercise Bionic registration with the selected VM already
running and verify the kernel routes afterward. Start the
[Host signal bootstrap](SIGNAL_BOOTSTRAP.md) before loading the VM.

## GNU Host lifecycle

During same-thread bootstrap the router checks the kernel's GNU Host
`SIGSETXID` action against its owning libc, retains its flags and restorer, and
routes entry through the original Host TCB. A set-ID broadcast can receive its
acknowledgement even while a Host pthread is executing Guest code. The router
verifies the entry while active and restores the exact kernel action on
shutdown. Each libc's credential broadcast covers its own registered
pthreads. Process-wide credential changes spanning both registries remain
outside the runtime contract.

Signal 32 stays on the owning GNU Host libc path. The carrier blocks it while
Guest TLS is installed. GNU native callers that can be cancelled must disable
Host cancellation through their bridge scopes and restore their previous
state after retirement. The native fixture cancels genuine GNU Host pthreads,
fires Host `SIGEV_THREAD` timers, and broadcasts the existing GID while both
carriers run Guest code. It also requests cancellation while a carrier's
cancellation is disabled, then checks cleanup on the Host TCB after retirement.
It repeats native operations after restoring the kernel actions. Native forced
unwind through a suspended JNI or VM frame is outside this contract.

[GNU cancellation](https://github.com/bminor/glibc/blob/glibc-2.42/nptl/pthread_cancel.c)
and [GNU credential broadcast](https://github.com/bminor/glibc/blob/glibc-2.42/nptl/nptl_setxid.c)
define these private entrypoints.
