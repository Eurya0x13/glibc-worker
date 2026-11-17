# Guest asynchronous entry

[English](../en/GUEST_ASYNC_ENTRY.md) | [简体中文](../zh/GUEST_ASYNC_ENTRY.md)

Core supplies a normally returning entry primitive for a runtime-owned Guest
notification. This is a prerequisite for independent Guest asynchronous signals,
not a complete `sigaction`/`ucontext` implementation. The installed JNI service
does not yet enable this entry or consume its independent inbox.

## Entry and ownership

An integration reserves an explicit, unused public RT number through the
internal `signal_router_internal_action` bootstrap API. The route uses an
immutable kernel-selected generation and the existing TLS-free carrier lookup.
It restores Host TP and ARM64 x18 before invoking the integration callback, with
every catchable signal blocked. Normal return preserves Host errno. Public
registration, replacement and queries of the reserved route return `EBUSY`;
router verification, rollback and restoration retain the normal transaction.

Initialize/adopt the Host router and freeze recipient admission before reserving
the route. Its callback and owned data must outlive every selected or saved
frame. A raw registration outside this layer cannot be safely adopted later.
The number must differ from Host reservations, deferred fault notification lanes
and logical Guest signal numbers. Do not infer equal signal numbers from equal
`SIGRTMIN` offsets across libcs: each libc reports its public lower boundary
independently. The native fixture passes one explicit kernel number and checks
the actual Host registration instead of assuming an available range.

The [target registry](SIGNAL_TARGETS.md) owns admission after actual TCB bind,
shared selection and entry pins. Targets are registered Host carriers, including
while their Guest domain is executing. This route is not a Host callback entry
for genuine GNU pthreads.
That path still requires its own Guest-native lifecycle/disposition integration.

The callback can call `domain_guest_signal_stack_call` with a valid Guest TCB,
Guest FP/x18 state and a separately owned guarded entry stack. It changes stack
and thread pointer, invokes Guest-compiled code, and restores Host stack,
TP/FP/x18 on normal return. Neither domain calls the other libc's allocator.
The integration must keep catchable signals blocked across these transitions.
The primitive does not invent a Guest `ucontext` from a Host kernel frame or
make an uncoordinated nonlocal exit safe.

## Wait recovery

Before executing hidden work, the integration calls
`gw_signal_selection_hidden` on the actual interrupted Host kernel frame.
It therefore retains the existing wait's deadline, native remainder and visible
Host interruption rules. Work in Guest TLS is charged to that same Host budget.
An empty/stale notification uses this hidden path too. The selected private
kernel action has `SA_RESTART`; that flag alone is insufficient for sleep/ppoll
transparency, so the explicit wait adapter remains necessary.

This internal entry does not add private-bit mask virtualization,
Guest dispositions/default actions or consuming waits. The optional
[owning-libc send extension](GUEST_SIGNAL_SEND.md) now supplies tagged public
sends in the fixture. The test integration explicitly controls its notification
mask and senders. General
blocking I/O and private/raw Host waits retain the limits in
[signal waiting](SIGNAL_WAITING.md).

## Reproducible native validation

Use a matching adoption runtime and an explicitly reviewed notification number:

```sh
python3 core/tests/guest_async.py --runtime "$DOMAIN_RUNTIME" --notification 55
python3 core/tests/guest_async.py --runtime "$DOMAIN_RUNTIME" --notification 55 --ubsan
python3 core/tests/guest_async.py --abi arm64-v8a \
  --runtime "$DOMAIN_RUNTIME_ARM64" --notification 55 \
  --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

The runner builds an installed core SDK and a separate GNU executable, then
launches the existing dynamic loader in one process. Host and Guest compare
shared structure sizes and offsets before admitting recipients; the Guest
checks the exact private libc capability value. Queue operations are compiled
with, and executed by, Guest libc. The original Host signal action for the same
logical number remains installed and receives its own untagged kernel packet.
The optional native `--ubsan` run instruments Host/core with Clang; the GNU
Guest and its libc remain uninstrumented. It uses a separate output directory.

Two carriers over two joined lifetimes consume 28 private/shared Guest packets
with sender, code, value and queue-source checks. Guest entry interrupts Host
busy code without Guest-call polling, runs during an actually blocked native
sleep and `ppoll`, and interrupts
a Guest busy loop identified by its real kernel-saved PC. Each entry checks TID,
Guest TP/TLS, guarded-stack placement, errno, FP control and Guest allocation.
Both timed Host waits complete transparently; retry observations prove 150 ms
of Guest work reduces their original 500 ms budgets.
Each actual `ppoll` also receives an intentionally empty notification and must
re-enter its kernel wait before the real Guest packet is published.
After send admission closes, the target drains its helper, completes Guest
teardown and joins before any TCB, stack or receiver queue is reclaimed. A
post-close publication returns `ESRCH`.

Source/artifact hashes, build/run commands, device identity and structured
results live under `core/build/tests/guest-async{,-android}/`. The summary keeps
`production_ready`, `guest_public_signal_abi`, `jni_vm_validated` and
`nonlocal_async_entry_validated` false. Test-owned stacks and senders establish
quiescence only for this normally returning native fixture. They do not prove
independent module signals during Java, saved-frame resumption, genuine GNU
thread targets or a live VM/ART reclamation provider.
