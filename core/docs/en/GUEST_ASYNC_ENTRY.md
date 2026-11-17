# Guest asynchronous entry

[English](../en/GUEST_ASYNC_ENTRY.md) | [简体中文](../zh/GUEST_ASYNC_ENTRY.md)

Core supplies entry and destructive recovery for a runtime-owned Guest
notification on an adopted carrier. The native fixture exercises normal return
and Host-target `siglongjmp`. The opt-in installed JNI service adds tagged
delivery on adopted carriers and GNU workers.

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
while their Guest domain is executing. Genuine GNU pthreads use their own
native entry for Guest packets and the explicit [Host relay](HOST_SIGNAL_RELAY.md)
for Host packets.

Initialize a `gw_signal_execution` from the valid owning Guest TCB with
`gw_guest_signal_execution_init`. Compile that source and `start.S` with Guest
libc. Provide guarded stacks for execution, alternate delivery and abort.
Execution and alternate delivery may use the same mapping. The abort stack
must be disjoint, and the invocation and its stacks must outlive all active
frames and any saved continuation.

`signal_guest_execute` switches into a Guest checkpoint while delivery is
blocked. It installs the Guest physical alternate stack after leaving the Host
stack. A Host-origin entry supplies its actual kernel frame; a Guest-origin
entry passes NULL and retains the outer Host anchor. Nested Host handlers
receive the suspended Host PC, registers and logical mask. The saved Host SP
from the cooperative switch bounds the stack space available to those handlers.
Current Host FP control and ARM64 x18 are restored before Host dispatch.

A private notification uses the currently interrupted stack for its kernel
frame. A Host handler can run on its ordinary stack while an earlier routing
frame still occupies the Host alternate stack. An extra `SA_ONSTACK` on the
private helper would instead reuse its occupied top. The application action's
own `SA_ONSTACK` choice is applied when forwarding the Host handler.

On Host-target `siglongjmp`, the Host native scope walks the abandoned suffix
from the innermost execution outward. Each scope resumes Guest through a
separate abort stack and its owning libc's checkpoint. Guest releases its
bridge-owned resources with signals blocked and switches to a fresh Host
landing. The landing restores Host stack configuration and unlinks its anchor
before continuing to the requested Host checkpoint. Active entries prevent
retirement. A separate quiescence contract covers kernel-selected entries and
saved contexts.

## Installed JNI integration

When an adopted carrier cooperatively yields, the GNU service saves a live
`getcontext` on the Guest stack. Tagged delivery during Host or Java execution
uses this snapshot and the actual suspended Guest TP. If a Guest instruction
was interrupted and a Host handler is running, the router instead supplies
that newer kernel frame and the stack floor saved by the switch. Nested Guest
handlers use their normal or alternate stack below live frames.

For a Guest-target `siglongjmp` through the reviewed native `GetVersion`
callback, the signal invocation first exits through its GNU checkpoint. Host
then retires the abandoned native scopes and publishes the callback response.
The original Guest RPC cleans up before the owning glibc resumes the target.
The target's saved-mask mode controls its final mask. See the
[installed SDK tests](../../../android-jni/docs/en/SDK_TESTS.md).

## Wait recovery

Before executing hidden work, `signal_guest_execute` calls
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

Two carriers over six joined lifetimes consume 84 private/shared Guest packets
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

Sixty nested Host deliveries check original Host contexts, mask edits, TP/TLS,
floating-point control and ordinary/alternate-stack placement. The six batches
cross disabled, enabled and AUTODISARM Host alternate stacks with Host
`SA_ONSTACK` on/off, including native stack-query flags inside the handler.
Each carrier also receives a real Host signal during hidden Guest work in a
native `ppoll`; that wait must return `EINTR`. A separate disposable control
deliberately omits the Host anchor and must observe the wrong context, proving
that a successful Guest call alone would miss this failure.

Twelve Host-target jumps abandon three nested Guest executions each. Each
Guest finish callback releases its own target pin in reverse entry order. Host
verifies the original mask, TLS and alternate-stack state, including native
AUTODISARM behavior. A subsequent notification, ordinary Guest call and
recipient teardown must succeed. Each invocation is single-use.

Source/artifact hashes, commands, device identity and structured results live
under `core/build/tests/guest-async{,-android}/`. The native test records
`host_jump_guest_retirement` and `retired_async_entries` for the covered jump
path. The broader `nonlocal_async_entry_validated` remains false pending
Guest-target and saved-context recovery. The test also leaves
`production_ready`, `guest_public_signal_abi` and `jni_vm_validated` false.
