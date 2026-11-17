# Cross-domain nonlocal recovery

[English](../en/NONLOCAL_RECOVERY.md) | [简体中文](../zh/NONLOCAL_RECOVERY.md)

Transparent cross-domain recovery is a first-release requirement for same-thread
execution. This document specifies acceptance criteria, not a completed
capability. The opt-in native unwind integration covers a subset of the contract;
it must not be enabled as complete nonlocal compatibility.

The contract covers valid native continuations across libc domains, including
bridge-owned JNI resources. It does not cover native transfers that skip
Java/VM frames. Normal JNI return, Java exception propagation and surviving
outer Java frames must remain correct; cross-libc execution must not introduce
failures in those supported paths.

## Native unwind integration

`glibc_worker::nonlocal_interpose` preserves opaque Host libc jump buffers and
tail-calls native capture functions. Call `gw_nonlocal_init()` during serialized
bootstrap, before recipients or the VM are created. Host jump restoration walks
the abandoned bridge scopes on their ordinary stacks with catchable signals
blocked. Android-jni's matching Guest service uses its own libc jump entry to
retire the interrupted native invocation. JNI cleanup runs on the restored Host
stack before the final native restore. This is destructive unwind, not suspended
continuation resumption.

The android-jni build option `GW_NATIVE_UNWIND_INTEGRATION` remains off by default.
Both the Host library and Guest service must select it; their private transport
capability handshake rejects mismatches. Consumers explicitly link the core
interposition target. The validation fixture is described in
[the installed SDK test](../../../android-jni/docs/en/SDK_TESTS.md).

The fixture exercises Host-target recovery while Guest native code is executing,
including a surviving outer Java callback, saved/unsaved masks, handler-local
jumps, alternate stacks, subsequent calls and teardown. It does not jump across
Java frames. A separate core test covers glibc's fortified `__longjmp_chk` entry;
the final restore retains libc's check rather than substituting an unchecked jump.

With tagged delivery enabled, the installed service also retires nested
Guest handler activations before a native jump reaches the original Guest
command through `GetVersion`. Saved and unsaved mask cases are included in
the [adopted-carrier fixture](../../../android-jni/docs/en/SDK_TESTS.md#independent-guest-delivery-on-adopted-carriers).

The owning Guest libc reports synchronous context capture and restore at its
native assembly's mask operation. A recorded `setcontext` target can be the
original native command or an older live Guest handler. The coordinator retains
the source context bytes before retirement and calls the owning libc after
the abandoned scopes have released their resources. The Linux Host interposer
records `getcontext` before tail entry and handles `setcontext` through the
same Host scope chain. Native context restoration retains each libc's machine
state and uses the target's logical mask.

GNU Host `swapcontext` records the source and tail-calls native libc. It can
enter a fresh context prepared on another stack while retaining its own scope.
Guest `swapcontext` retains native same-scope resume behavior and returns
`ENOTSUP` for a switch to an older bridge scope. Supporting resumption across
bridge scopes requires lifetime accounting for both owning stacks.
Android verification covers Guest glibc contexts and Bionic jump buffers.

The native-call integration masks catchable signals while acquiring/releasing
bridge resources, publishing commands and crossing TP/stack boundaries. Requests
not yet executed can be retired by their sole carrier; completed requests do not
re-enter Guest abort. Active abort must match the exact Guest command, not an
outer invocation's checkpoint. These rules cover regular native calls, not every
loader/lifecycle or VM-entry window.

Signal entry restores the suspended Host floating-point control environment
before invoking a Host handler from Guest. The original signal frame supplies
Guest control state for the abort entry. This snapshot is separate from the
cooperative switch target: a signal may occur after TP changes but before the
target's floating-point environment loads. Mutating that target would corrupt a
normal return. ARM64 also retains interrupted x18. This does not establish full
SVE/SME, PAC/SCS or x86 CET compatibility.

The opt-in integration also keeps the Host mask on its suspended Host frame
and the Guest mask with its adopted context. A Guest JNI callback and nested
native reentry retain each side's mask; a Host handler entered from Guest uses
the saved Host mask together with its registered action mask and `SA_NODEFER`.
If that handler abandons Guest through the tested Host-target jump, the Guest
mask comes from the interrupted kernel frame. In active adopted calls, the
patched `pthread_sigmask`/`sigprocmask` preserve the suspended Host's physical
mask for reserved asynchronous signals and maintain Guest logical mask state.
The owning glibc's `getcontext`, `setcontext` and `swapcontext` assembly use the
same adapter while retaining native register, FP and protection-state code.
Unbound GNU pthreads retain their native syscall path. Host-blocked reserved
signals stay in the kernel pending queue with original sender information,
realtime FIFO ordering and delayed one-shot consumption.

The [signal wait adapter](SIGNAL_WAITING.md) also translates temporary masks in
`sigsuspend`, `ppoll`, `pselect` and both epoll mask-wait APIs. Consuming waits,
pending queries and Guest signalfd creation/update exclude Host-reserved signals.
Two additional Host-target jumps first observe a real kernel suspend wait,
then check saved/unsaved masks, explicit fixture cleanup, another JNI call and
teardown. They do not add Guest asynchronous pending queues.

Managed Host signalfd reads and consuming waits have an additional unwind
scope when jump interposition is initialized. A signal arriving after native
consumption but before decoding first moves consumed originals into that scope;
fresh signals queued by the handler remain distinct. Outside jumps retire the
scope on its ordinary stack, and native cancellation retains libc cleanup.
The installed JNI fixture checks read/readv abandonment, context release and
another Guest call. This is destructive jump recovery, not general Host
`setcontext`, suspended-context resumption or continuation reclamation.

The opt-in service has independent Guest synchronous-fault dispositions. Core
retains routing entries for all six fault numbers even while Host selects a
default or ignored action; Host one-shot consumption occurs only on Host
delivery. Adopted carriers restore Guest TP/FP/x18 before entering its handler.
GNU-created pthreads keep their real Guest TP and never acquire a fictitious
Host/VM thread identity. Positive tests cover real SEGV/BUS/ILL/TRAP faults and
seccomp SYS traps,
Guest-native jump/context recovery, nested handlers and subsequent JNI calls.
Separate processes verify default fatal actions with actual signal termination.

The opt-in integration keeps independent native alternate-stack configurations.
With delivery blocked, each domain installs its own configuration after SP/TP
have left the other domain's stack. The kernel builds Guest signal frames on
the Guest-selected stack. Host handlers forwarded from Guest execute on the
suspended Host stack or its alternate stack according to the Host action.
The outer fault route requests an alternate frame; a logical action without
`SA_ONSTACK` executes on its interrupted stack, preserving the x86 red zone.
Queries retain native `SS_ONSTACK`, `SS_DISABLE` and `SS_AUTODISARM` behavior.

The owned-stack tests cover normal return, nested handlers, saved/unsaved jumps,
`setcontext`, and an AUTODISARM handler suspended with `swapcontext` while Guest
performs Host callbacks and receives another fault. They resume that handler
before releasing its stack. Eight additional Host-target jumps abandon Guest
handlers across combinations of both domains' stack settings; explicit fixture
cleanup then checks Guest state, subsequent calls and module/runtime teardown.
Host AUTODISARM state is captured before the Guest abort path, so an abandoned
Host handler's disabled stack is not silently rearmed.

This is not full independent signal virtualization. General saved-continuation
lifetime accounting and reclamation remain unfinished. Tagged asynchronous
sends/pending queues, Host delivery on GNU
pthreads, mask/wait APIs beyond the tested paths and transition-window reentry
still need the complete signal design. Passing Guest fault recovery within a
native invocation does not establish unwinding a live Host callback to a Guest
checkpoint or resuming arbitrarily saved signal frames.

Remaining obligations include general Guest-target integration, resumable native
contexts, state-publication windows, complete architecture-state restoration and
safe JNI resource cleanup. Host scope exit releases its capture slots for reuse.
The thread retains the record pages and its captures made outside bridge scopes.
Unknown captures,
stale scope generations and nested
pending transfers currently fail the integration rather than recover. Those
limitations are unresolved work, not the accepted first-release semantics.

The matching opt-in service also coordinates Guest-target native jumps at a
reviewed C-only `GetVersion` callback boundary. Its glibc hook observes capture
after native register saving, and restore before native cleanup and mask
restoration. Records belong to a live native invocation generation. Invocation
exit invalidates its records, slots are reused, and carrier teardown unmaps its
high-water record pages. Host capture-page reclamation and general resumable
continuations still require the remaining lifetime protocol.

An inner Guest jump first retires the abandoned Guest command. Host scope
routing then retires the corresponding command, trampoline and C callback
suffix. Suspended Guest RPCs return through their ordinary protocol and metrics
cleanup before the owning glibc restores the destination buffer. Fortified
calls finish through `__longjmp_chk` on the original Guest stack, not an
unchecked substitute on a disjoint scratch stack. Public module interfaces
remain `sigsetjmp`/`siglongjmp`; no module-specific macros are needed.

The installed fixture uses a real JNIEnv and bridge trampolines, with a test
callback inserted before the cached Host `GetVersion` dispatch. It covers
saved/unsaved masks, one/three/five nested C boundaries, 300 live captures,
repeated buffer reuse, a fortified source entry, subsequent calls and normal
module/runtime teardown. Other native JNI callback boundaries with temporary
ownership, valid Guest-target native continuations and cross-domain `setcontext`
remain required work. Skipping Java/VM frames is outside this contract.

The core adoption fixture additionally tests a controlled Guest-target transfer:
Host resumes Guest on a separate guarded stack and Guest's libc restores its own
live checkpoint. Per carrier it checks saved and unsaved `sigsetjmp` masks and
restores one `getcontext` snapshot twice with `setcontext`. The subsequent
ordinary calls and carrier teardown still run. This fixture preserves its Host
frames cooperatively; it does not provide asynchronous Guest signal delivery,
abandon a JNI/VM callback, or implement continuation lifetime management.
Run `core/experiments/same-thread/run.py --adopt --signals` with a matching
adoption runtime; Android validation additionally requires `--abi arm64-v8a`,
`--ndk`, and `--serial`.

## Native GNU thread unwind

Genuine GNU pthread hardware faults tail-enter the Guest-owned kernel handler
selected by `signal_guest_kernel_bind`. The outer assembly removes its temporary
Host frame before the transfer. Guest cancellation and `pthread_exit` can then
unwind through the actual kernel frame to interrupted GNU code, without asking
the GNU loader to find frame information in Bionic-only images. Host-owned
asynchronous signals keep the explicit Host dispatcher path. An unconfigured
GNU hardware-fault entry still fails closed instead of entering a VM handler.

The owning Guest's `domain_stack_call` includes DWARF rules for their preserved original stack register, including while SP
points at another stack. The native GNU unwinder retains its signal-restorer
recognition on
[ARM64](https://github.com/gcc-mirror/gcc/blob/releases/gcc-14/libgcc/config/aarch64/linux-unwind.h)
and [x86_64](https://github.com/gcc-mirror/gcc/blob/releases/gcc-14/libgcc/config/i386/linux-unwind.h).
This supplies native forced-unwind frame traversal; it does not turn arbitrary
cross-domain `longjmp` into C++ exception cleanup.

The installed JNI control tests cancellation at a handler's native `read` and
explicit `pthread_exit`, on ordinary and alternate signal stacks. Compiler
cleanup records in both handler and interrupted body must execute; another JNI
call, module close and runtime shutdown must still succeed. The separate
GNU-only control uses the same source without loading the bridge. Both compile
with `-fexceptions -fnon-call-exceptions`, so the faulting load is an actual
cleanup point under
[GCC's code generation contract](https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html#index-fnon-call-exceptions).
These flags describe the test oracle, not a new recompilation requirement for
existing modules. The control crosses no Java/VM frames and makes no adopted
carrier cancellation or complete independent asynchronous-entry claim.

## Observable behavior

A Host checkpoint can remain live while its carrier executes Guest code. A Host
signal handler may leave that Guest invocation by jumping to the checkpoint.
Recovery must restore a coherent Host continuation and retire the abandoned
bridge work. The carrier must remain usable for subsequent calls and orderly
module/runtime teardown. Equivalent Guest-target and nested cases require the
same treatment; implementation order must not reduce the final contract to the
Host-target case.

Existing modules retain their libc interfaces. Requiring modules to replace
`sigsetjmp`/`siglongjmp` with project-specific macros would change the requirement.
Each buffer belongs to the libc and logical thread that created it. Its layout,
pointer protection and architecture state must be handled by that libc's
reviewed adapter, never by passing a bionic buffer to glibc or vice versa.

The target must still be a valid continuation. Cross-carrier jumps, returned
stack frames and language-level undefined behavior do not become valid because
the bridge exists. A jump does not roll back application writes, close arbitrary
application resources or release application-owned locks.

## Restoration obligations

| State | Required behavior |
| --- | --- |
| Execution | Restore the target stack, continuation, ABI-preserved registers, TLS and applicable architecture protection state. |
| Signals | Preserve the chosen API's mask semantics, logical pending signals, handler nesting and alternate-stack behavior. Do not restore a saved mask for an API that did not save one. |
| libc | Use the target domain's TCB and buffer semantics. Do not invent checkpoint-time `errno` restoration that native `longjmp` does not promise. |
| Bridge frames | Reconcile command depth, Guest execution depth, `rpc_top`, active flags and request/response generations for precisely the abandoned suffix. |
| Ownership | Release bridge-owned module borrows, trampoline activity and temporary records exactly once; keep surviving outer calls and references live. |
| JNI | Reconcile bridge-owned local frames, reference tokens and in-flight resources through a safe VM path. A signal handler cannot simply call arbitrary JNI cleanup functions. |
| Lifetime | Prevent later resumption into retired stacks, TLS, signal entry generations or unloaded modules. Account for saved contexts that are still resumable. |

`setcontext` requires a separate continuation-lifetime model. Restoring a saved
context can resume suspended work; it must not automatically be treated as
destructive unwinding. Similarly, a signal-frame context is not interchangeable
with a synchronous `getcontext` snapshot. Supported capture/restore paths must be
identified and tested for each selected libc and architecture.

## Implementation boundaries

Core owns continuation identity, domain transitions and signal-frame lifetime.
The Guest runtime patch owns glibc capture/restore integration. Host integration
must cover the actual bionic/libc call paths, including relevant aliases and
fortified entrypoints; merely interposing one public symbol is insufficient.

Android-jni owns bridge/JNI resource reconciliation. Jvm-jni owns the reviewed
VM integration needed for supported calls and lifecycle operations. Normal JNI
returns and Java exception propagation must preserve VM state. Native transfers
that bypass Java/VM frames and their cleanup are outside this contract; the
bridge does not implement Java stack unwinding or require a custom JRE for it.
Java frames outside the abandoned native suffix must remain usable.

The restore path must define its signal reentrancy and commit boundary before it
publishes partially restored state. Resource cleanup that needs allocation,
locks or VM entry must not run arbitrarily inside a signal handler. Capture and
restore interception must preserve native calling conventions, including
returns-twice behavior and tail-return placement for checkpoint functions.

## Acceptance matrix

The out-of-scope managed-frame transfer is isolated by the
[VM-only native-transfer reproducer](../../../jvm-jni/docs/en/NONLOCAL_VM.md).
Its observations are boundary evidence, not a bridge compatibility pass or a
blocker for native recovery. The matrix below does not abandon Java/VM frames.

All rows remain required until demonstrated with reproducible results. They are
not a list of tests already passing.

| Case | Assertions after transfer |
| --- | --- |
| Host signal interrupts Guest and jumps to live Host checkpoint | Correct target TLS/stack; abandoned request cannot resume; next call succeeds. |
| Guest-target recovery across a Host callback | Correct Guest continuation and ownership; surviving outer frames remain usable. |
| Multiple nested domain crossings | Only the abandoned suffix is reconciled, in the correct order. |
| `sigsetjmp` with and without saved mask | Masks follow the selected API; nested/pending delivery remains correct. |
| Plain/aliased/fortified jump entrypoints | Real module call paths reach recovery; no alias silently bypasses bookkeeping. |
| `getcontext`/`setcontext` and supported context-switch paths | Distinguish resume from abandonment; preserve saved-context lifetimes. |
| Nested handlers and alternate stacks | Repeated transfer does not leave stale handler state or reclaim a live signal frame. |
| Interruptions around bridge state publication | No partially committed depth, response or ownership state becomes observable. |
| Live JNI references and module borrows | VM checks pass; balanced bridge-owned resources; application-owned resources retain native semantics. |
| Repeated recovery, subsequent call, close and shutdown | No accumulated active flags, orphan requests, stale tokens, leaks or teardown hangs. |
| Invalid/stale targets | Deterministic adapter behavior without corrupting another live continuation; these cases cannot substitute for valid-jump success. |
| x86_64 and Android ARM64 | Exercise actual matched libcs; desktop results alone do not establish Android compatibility. |

Signal registration reclamation must use this continuation-lifetime accounting
when proving quiescence. A zero handler-entry count cannot rule out later
resumption into a saved signal frame.

## Source references

- [glibc jump restoration](https://github.com/bminor/glibc/blob/glibc-2.41/setjmp/longjmp.c)
  performs libc cleanup, optional mask restoration and machine restoration.
- [glibc x86_64 register restoration](https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/x86_64/__longjmp.S)
  includes pointer demangling and conditional shadow-stack handling.
- [glibc x86_64 setcontext](https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/x86_64/setcontext.S)
  explicitly describes synchronous context-switch semantics.
- [JNI reference and exception ownership](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html)
  describes VM-owned local-reference registries and explicit resource release.

The runtime build manifest identifies the actual source used by each artifact;
source links explain mechanisms, not permission to substitute an unverified ABI.
