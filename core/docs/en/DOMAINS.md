# Same-thread domains

[English](../en/DOMAINS.md) | [简体中文](../zh/DOMAINS.md)

`glibc_worker::domains` owns architecture context switching, carrier signal
registration and Host signal routing. It links only core, the platform C library
and `libdl`; it does not discover Java, JNI, libffi, ART or HotSpot.

`domain_context.h` describes the saved register ABI. Applications normally use
an upper-layer runtime API rather than constructing these records. An adopted
Guest has a protected stack and a real GNU TCB/DTV allocated by the matching
patched libc. Crossing domains switches stack and thread pointer; it does not
share libc allocator objects, `FILE`, pthread objects or TLS addresses.

## Runtime artifacts

Build the ordinary matched source/dependencies first, then run:

```sh
python3 core/scripts/build-domain-runtime.py --abi arm64-v8a --jobs 4
```

The recipe publishes `core/build/domain-runtime/<ABI>/<key>/`. Its manifest binds
ABI, source/patch inputs and the private adoption ABI. The service checks
`__gw_domain_capabilities` before admitting JNI calls. A libc without that entry
or with another value is rejected. This is capability negotiation, not a claim
that any arbitrary GNU distribution can be embedded without validation.

The maintained patch fragments live in `core/src/runtime/`; the recipe and
its generated source/object trees remain owned by core. `experiments/` contains
validation entrypoints and historical evidence, not the implementation used by
the installed domain target.

## Signal integration

The regular domain target does not export libc's `sigaction`, `signal` or
`sigaction64`. Early bootstrap integrations can explicitly link
`glibc_worker::signal_interpose` and initialize the router before creating a VM.
Late integrations must route their reviewed signal chain explicitly. A JVM/ART
artifact profile and import patching belong to jvm-jni.

The explicit bootstrap target also routes `siginterrupt`, `sigignore` and
`sigset`; GNU builds additionally route `bsd_signal`, `ssignal`, `sysv_signal`,
`__sysv_signal` and `__sigaction`. Helpers must not replace a kernel entry through
libc's hidden registration calls. BSD restart policy and action masks follow
the carrier's libc: GNU and bionic `signal()` behavior is checked separately.
A consumed one-shot retains its original flags/mask, as native queries do.

Host registration routing covers every catchable public signal, including
`SIGUSR1`, `SIGALRM`, `SIGCHLD` and unreserved RT signals. A routed Host action
does not add an exclusive VM reservation; `signal_router_reserved()` reports
the selected ownership profile plus private notification signals. Libc-private
signal numbers remain outside this public layer. Existing custom actions require
explicit adoption before Guest admission. `signal_router_guest_ready()` returns
`ENOTSUP` for an unadopted custom action, without changing it. Bootstrap consumers
can initialize and adopt before VM creation; late consumers retain the reviewed
outer-chain requirement.

The native comparison fixture exercises each helper and verifies the kernel
route after every mutation:

```sh
python3 core/tests/signal_aliases.py
python3 core/tests/signal_aliases.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

This target covers executable bootstrap interposition. It does not expand a
late-loaded VM profile to unreviewed imports or guarantee interception of
explicit libc handles and raw syscalls.

The agreed ownership policy is:

1. A synchronous hardware fault belongs to the domain executing the faulting
   instruction. This takes precedence over asynchronous reservations: a Guest
   SIGSEGV must not enter the VM's implicit-exception machinery.
2. VM-reserved asynchronous signals belong to Host. A runtime-tagged Guest send
   to a reserved signal fails with `EBUSY`, without signaling the VM.
3. Other signals explicitly tagged by the Guest runtime belong to Guest.
4. Untagged external signals belong to Host, including process-directed signals.

`signal_contract.h` implements and tests this classification separately from
actual signal delivery. The ordinary service enforces Host reservations. The
explicit native-unwind service additionally installs separate Guest synchronous
fault dispositions and adopted-carrier masks. Its `signal_router_guest_faults`
integration retains outer fault routes for Host default/ignore actions and
consumes Host one-shot handlers only on Host delivery. GNU-created pthreads can
recover synchronous faults under their real Guest TLS. Host-reserved pending
asynchronous signals retain their kernel queues while Guest executes.

These are partial mechanisms, not independent dual-domain signal delivery.
Independent native signal stacks and the tested AUTODISARM suspend/resume paths
are available in the opt-in service. Its [signal wait adapter](SIGNAL_WAITING.md)
protects Host-reserved queues across mask-and-wait, consuming waits, pending
queries and signalfd creation/update. Tagged asynchronous sends, general Guest
pending queues and Host delivery on GNU pthreads remain incomplete. See
[nonlocal recovery](NONLOCAL_RECOVERY.md) and the repository `todo.md` for the
required final contract. The regular SDK does not silently enable the opt-in
unwind capability.

Raw signal syscalls that bypass the runtime cannot supply the logical domain
identity. There is also no implicit Host TLS/JVM thread identity on a GNU-created
pthread. Such a thread must not run a Host handler with its Guest TP installed.

Signal dispositions are immutable per registration generation. The optional
`signal_router_reclaim` API releases retired generations only under a supplied
recipient-quiescence gate. The provider must stop new recipients/registrations,
prove every existing recipient has left routing entries and saved signal frames,
and prohibit a later jump into those frames. It runs with the writer lock held
and catchable signals blocked; it cannot reenter the router or wait for resumed
threads to acquire that lock. A failed gate leaves all mappings unchanged.
Current published actions remain alive; after restore the same gate can release
the remaining records. Without this provider, entries remain pinned. A general
JVM safepoint is not, by itself, a valid provider. Handler entry counters
alone cannot justify freeing records: Linux can select the handler and construct a
user signal frame before its first user instruction runs. A thread may also
leave a handler through a nonlocal exit. The standalone
contract validates reclamation with all owned workers joined, failure while an
old delivery is paused, delivery after collection, and a completed Host-side
`siglongjmp`. It does not prove a live JVM/ART-wide quiescence implementation or
support for abandoning a Guest call through a cross-domain jump.

Transparent cross-domain `siglongjmp`/`setcontext` recovery is required for the
first release. Its restoration obligations and acceptance matrix are specified
in [nonlocal recovery](NONLOCAL_RECOVERY.md). Same-domain-only success or
quarantining a valid cross-domain jump does not complete that requirement.

Relevant kernel paths are `get_signal`/`SA_RESETHAND` publication and
`setup_return`/`setup_rt_frame` on ARM64:

- [Linux signal selection](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)
- [ARM64 signal frame construction](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c)

## Lifetime

A checked bootstrap failure before clone reclaims temporary mappings and returns
an errno value. After clone, the loader and supervisor stack stay pinned even if
the service handshake fails. A userspace completion flag cannot prove kernel
thread exit. The embedding application must dispose of a quarantined process;
core does not kill it automatically or unload code under saved continuations.
