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

The adoption runtime uses glibc's `mmap` heap fallback. The process break is
shared with Host even when the Guest's loader starts libc as its first
instance. Guest libc allows `sbrk(0)` queries and reports `ENOMEM` on requests
to move the break. See [glibc's secondary-libc allocator policy](https://github.com/bminor/glibc/blob/master/malloc/arena.c).

The owning libc's [logical-mask lifecycle](MASK_LIFECYCLE.md) separates native
GNU thread inheritance from a borrowed carrier's physical mask and supplies
an optional observer for queue eligibility. It requires the matching private
capability; it does not create Bionic state for GNU threads.

## Signal integration

The regular domain target does not export libc's `sigaction`, `signal` or
`sigaction64`. Early bootstrap integrations can explicitly link
`glibc_worker::signal_interpose` and initialize the router before creating a VM.
Late integrations must route their reviewed signal chain explicitly. A JVM/ART
artifact profile and import patching belong to jvm-jni.

The opt-in target retains its archive objects through project-owned entrypoints.
A libc-named linker reference alone is insufficient: a weak implementation from
a sanitizer runtime can satisfy it before our archive is examined. This target
supplies strong wrappers; it does not compose arbitrary third-party interposers.

The explicit bootstrap target also routes `siginterrupt`, `sigignore` and
`sigset`; GNU builds additionally route `bsd_signal`, `ssignal`, `sysv_signal`,
`__sysv_signal` and `__sigaction`. Helpers must not replace a kernel entry through
libc's hidden registration calls. BSD restart policy and action masks follow
the carrier's libc: GNU and bionic `signal()` behavior is checked separately.
A consumed one-shot retains its original flags/mask, as native queries do.

Host registration routing covers every catchable public signal, including
`SIGUSR1`, `SIGALRM`, `SIGCHLD` and unreserved RT signals. A routed Host action
does not add an exclusive VM reservation; `signal_router_reserved()` reports
the selected ownership profile plus private notification signals.
GNU Host NPTL remains native; Bionic private registrations use reviewed
[owning-libc routing](PRIVATE_SIGNALS.md), while Guest NPTL leases 63/64.
Existing custom actions require explicit adoption before Guest admission.
`signal_router_guest_ready()` returns
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

`signal_contract.h` implements the classification. The ordinary service
enforces Host reservations. The native-unwind build adds separate Guest fault
dispositions, adopted-carrier masks, and native signal stacks, including tested
AUTODISARM suspend/resume. Its mask adapter keeps Host-reserved pending signals
in their kernel queues. Its fault router consumes Host one-shot actions only on
Host delivery. Real GNU pthreads recover synchronous faults with their own Guest
TLS.

Explicit V2 startup connects tagged GNU libc sends and consuming waits to the
Guest inbox, with directed/shared queues and realtime accounting. On the
same-thread backend, the target carrier can enter a Guest handler while running
Host code or Java. Guest signalfd supports logical reads, readiness and descriptor
lifecycle. Host waits recover from internal notifications along the adapted
paths. See [Guest sends](GUEST_SIGNAL_SEND.md),
[asynchronous entry](GUEST_ASYNC_ENTRY.md), and [signal waiting](SIGNAL_WAITING.md).

Host arrivals on genuine GNU pthreads use a dedicated Host pthread.
The source signal frame remains live until the dispatcher returns. Shared
job-control signals follow Linux process semantics. See the
[dispatcher contract](HOST_SIGNAL_RELAY.md) and
[shared job control](SIGNAL_WAITING.md#shared-linux-job-control).

Default same-thread startup selects the execution backend. The independent
Guest signal paths require the [explicit V2 configuration](../../../android-jni/docs/en/RUNTIME.md).

Raw signal syscalls that bypass the runtime cannot supply the logical domain
identity. There is also no implicit Host TLS/JVM thread identity on a GNU-created
pthread. Such a thread must not run a Host handler with its Guest TP installed.

<a id="reclaiming-host-registrations"></a>

### Reclaiming Host registrations

Each kernel routing entry binds one immutable registration. Linux can select
that entry before the recipient executes its first instruction. A VM
safepoint or a zero active-handler count leaves that window open.

The optional `gw_host_signal_reclaim_enable` API leases one unused public
realtime signal after router setup and before starting relay or Guest helpers.
Call `gw_host_signal_reclaim` from normal Host control code with a timeout in
milliseconds. The lease and its code belong to the process until exit. Keep
the number available to native delivery and exclude it from application
signals, consuming waits and descriptors.

The collector detaches the retired batch under the registration lock, then
lets recipients progress. It compares pinned Linux task identities with the
kernel's thread count and requests an acknowledgement on each actual
recipient, including VM internal threads and unattached pthreads. Every
acknowledgement must belong to the same pinned identity. A missing reply or an
unstable census returns `EBUSY` without freeing the batch. A subsequent
attempt takes a fresh census.

With complete routing, ordinary generations can enter the live batch even if
their registration predates the realtime lease. The probe inspects the actual
interrupted PC and retries when it finds an entry that has yet to bind. Linux
may build several signal frames before executing the first entry. Before a
nested callback can see its native context, the gateway follows those frames
and keeps the corresponding unbound generations alive. The embedding's full
barrier accounts for any copies of those saved PCs. [Linux x86 return to userspace](https://github.com/torvalds/linux/blob/v5.15/kernel/entry/common.c),
[Linux ARM64 signal frames](https://github.com/torvalds/linux/blob/v5.15/arch/arm64/kernel/signal.c).

Ordinary routes preserve the action's public mask, kernel restart and one-shot
flags, and the actual temporary mask in effect at delivery. Native libc-private
signals are held through the entry prefix and their original mask is restored
before the callback. Callbacks and parked stacks use local action snapshots
after binding. Fully masked fault entries and data-only records remain eligible
under their existing proof. The embedding's full `signal_router_reclaim`
barrier also covers entries published before the router fully owned the native
chain or before it acquired the necessary libc-private masks. The native census
assumes one Linux thread group and ordinary thread creation, where new tasks
start without their parent's saved signal frames.

Wrapped Host waits handle the private acknowledgement as a hidden
notification. Enable live collection where the embedding can accept
`EINTR` from native non-restartable waits outside the adapters.
The original full barrier remains available for stopped VM integrations and
after router restore. Its provider must freeze new recipients and
registrations, cover saved continuations and avoid waiting on the writer lock
held by the collector. The standalone contract tests this path with joined
workers, a refused collection, subsequent delivery and a Host `siglongjmp`.

Native cross-domain `siglongjmp`, `setcontext` and supported `swapcontext`
paths follow the continuation lifetime and cleanup contract in
[nonlocal recovery](NONLOCAL_RECOVERY.md).

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
