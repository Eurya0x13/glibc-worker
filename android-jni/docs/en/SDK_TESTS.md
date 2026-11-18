# Installed runtime consumer checks

[English](../en/SDK_TESTS.md) | [简体中文](../zh/SDK_TESTS.md)

This fixture consumes the separately installed core, bionic-dlopen and
android-jni packages. It embeds an explicitly selected HotSpot artifact, loads a
glibc JNI module and verifies calls, references, close and shutdown. It does not
use ART as a substitute for an Android JRE.

`python3 android-jni/tests/runtime_sdk.py --help` lists desktop inputs. The Android equivalent is
`python3 android-jni/tests/runtime_sdk_android.py --help`; it additionally requires an explicit device,
NDK, Android JRE asset/library directories and matching VM startup profile. The
runner validates the selected libjvm/libjava hashes before deployment. Android
execution uses a shell process through linker64, not an installed APK; passing
does not establish app sandbox compatibility.
`--ffi` selects GNU Guest libffi (`core/build/ffi-linux/<ABI>`), including on
Android. The bionic Host archive under `ffi-android` is not a GNU service input.

The positive default/worker cases reject two malformed bridge configurations
before process ownership is claimed, verify `GW_HOST_NEW`, unchanged masks and
relay ownership, then successfully start the actual runtime. Two additional
`borrowed-relay` cases supply an already running Host dispatcher for each
backend. Shutdown must leave borrowed dispatchers running and join owned ones;
the external owner then stops its dispatcher before VM/router teardown.
The `runtime-relay-lifecycle` observation is required for positive cases,
including native recovery. Incompatible libc rejection must instead retain the
dispatcher and quarantine the already claimed bridge.

The deferred Host mask case uses the installed public
`gw_host_signal_channel_enable` API before VM creation. Core's separate native
ordering/context and failed-restore suites cover the consumer transaction;
see [the Host channel contract](../../../core/docs/en/SIGNAL_WAITING.md).

## Native unwind validation

Four `guest-signal-unwind` observations additionally require native GNU thread
cancellation/exit from a hardware-fault handler, ordinary/alternate stacks,
cleanup of the interrupted body and handler, another JNI call and teardown.
The source uses DWARF cleanup attributes, with exception support for faulting
loads, so a C-only cancellation jump fallback cannot hide skipped cleanup.
Run the same GNU-only oracle independently before attributing a failure to the
bridge:

```sh
python3 android-jni/tests/guest_signal_unwind.py --runtime "$GW_RUNTIME"
python3 android-jni/tests/guest_signal_unwind.py --abi arm64-v8a \
  --runtime "$GW_RUNTIME" --serial "$ANDROID_SERIAL"
```

The second command requires the GNU ARM64 compiler and an authorized shell
device. Neither control loads a JVM or bridge. Its result, compiler command,
runtime/binary hashes and raw output live under `build/tests/guest-signal-unwind`
or `build/tests/guest-signal-unwind-android` in this project.

Build and install android-jni with `-DGW_NATIVE_UNWIND_INTEGRATION=ON`. Run the
consumer with `--native-unwind --nonlocal`; the runner links the opt-in core
interposition target and builds a matching Guest service. Omitting `--nonlocal`
checks ordinary same-thread calls, the worker backend and rejection of an
ordinary libc without the adoption capability.

For deterministic publication/cleanup interruptions, build the test SDK with
`-DGW_NATIVE_UNWIND_TEST_HOOKS=ON` and add `--unwind-windows` to the runner.
Normal builds omit these hooks. The executable supplies a weak test callback;
a missing callback cannot pass the window assertions. Test SDKs are validation
artifacts, not deployment replacements.

With both options enabled, `--unwind-windows` also requires ten Guest-target
observations: two rounds each of saved/unsaved one-level jumps, a three-level
jump, a five-level jump with 300 live captures, and a fortified three-level
jump. A weak test callback reenters a native trampoline before the Host's
cached `GetVersion` dispatch; no Java frame is crossed. Each observation checks
the original Host mask, another Guest call and absence of JNI exceptions. Final
close/shutdown assertions cover the accumulated active bindings and commands.
Missing hooks or missing observations fail the case; a passing Host-target
observation alone does not satisfy this test.

### JNI result recovery

`--unwind-windows` also exercises 19 Host-target interruptions after a real
JNI result has been acquired and before its response commits. Cases require
balanced local/global/weak references, reflection locals, strings, all eight
primitive array element types, critical strings and arrays, a local frame, and
a reentrant JNI monitor. The UTF case preserves the exact pending exception.
The array case changes an unpublished cached copy, checks the Java array is
untouched, and reacquires the same cache buffer on the next call.

Guest-target `siglongjmp` and `setcontext` each abandon three nested C callback
boundaries. The test requires last-in-first-out cleanup, intact outer
references, unchanged staged outputs, rejection of retired reference tokens,
further JNI calls, module close and runtime shutdown. A separate JNI call
enters real Java code and attempts `setcontext` from a reentrant Guest native
method. It must return `ENOTSUP`, execute the Java `finally` block and leave
the monitor released.

The nonlocal case performs 64 Host-target signal jumps. Half occur within a Host
callback nested inside a surviving Guest call. Half use the installed alternate
signal stack. The handler first performs a local jump, then abandons the inner
Guest invocation. Assertions check mask semantics, stack exit, another native
call, JNI exception state, module close, runtime shutdown and JVM destruction.
Signal 52 must be unused; raw Guest tgkill intentionally targets Host under the
external, untagged signal contract. VM-reserved signals are not repurposed.

An additional floating-point test sets distinct Host/Guest rounding modes,
interrupts Guest and checks both modes after recovery/reentry. With window tests,
eight more transfers cover trampoline admission, command publication, completed
response and cleaned trampoline, each at root and nested depth: 74 transfers in
total, 36 nested and 32 using the alternate stack. Guest entry counts distinguish
unexecuted cancellations from completed work. This is targeted window coverage,
not exhaustive interruption coverage.

The fixture also checks different Host and Guest masks during JNI callbacks,
nested reentry and later calls. A delivered Host signal is checked with action
masks, `SA_NODEFER`, normal return and Host-target escape. Add
`--signal-pending` to require a Host-blocked signal to remain pending while
Guest has it unblocked. This separate acceptance run also checks three queued
realtime signals in FIFO order, original sender PID/UID and payloads, and a Host
`SA_RESETHAND` action that must remain installed until Host unblocks. The
matched runtime retains these Host-reserved signals in the kernel queue; it
does not dequeue and reconstruct their sender information. Results use a
separate `-pending` directory.

The matching native-unwind service additionally checks Guest synchronous
`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGTRAP` and seccomp `SIGSYS` handlers, real Guest
TLS and pthread
identity, saved/unsaved jump masks, `setcontext`, repairing a fault and returning
to retry the instruction, `SA_RESETHAND`, nested `SA_NODEFER` faults, handler-local
jumps and edits to a signal frame's mask. Host default/ignore/one-shot actions
must survive Guest faults. Eight GNU-created pthreads concurrently perform
Guest `siglongjmp` or `setcontext` recovery without a fabricated Host TCB.
The JSON observation requires 15 Guest checks and eight Guest fault threads.
The SYS probe installs an irreversible filter only on the disposable carrier;
it traps a magic argument to `getpid`, leaving ordinary calls usable. It checks
the kernel's syscall number, architecture and call address in `siginfo_t`.

The two `guest-fpe` observations exercise an adopted carrier and a
GNU-created pthread. Each enables `FE_DIVBYZERO`, executes a floating-point
divide and checks `FPE_FLTDIV`, the Guest jump and FP environment restoration.
Desktop requires `source: "hardware"`. AArch64 exception trap enables are
optional. If `feenableexcept` reports that the enable failed, the tested
thread asks Linux to deliver a native `FPE_FLTDIV` frame to itself. This
case requires a correct signal frame and recovery, with
`source: "kernel-self"`. Reports distinguish the two sources and require a
subsequent ordinary JNI call.

Nine Guest registration cases additionally cover `signal`, `bsd_signal`,
`ssignal`, `sysv_signal`, `__sysv_signal`, `__sigaction`, `siginterrupt`,
`sigignore` and `sigset`. Eight execute a real trap, validate Guest TLS/errno and
handler masks, then recover with Guest `siglongjmp`. The ignore case queries its
disposition without executing an ignored synchronous fault. A Host one-shot
trap handler must remain unchanged throughout. Individual JSON observations,
another JNI call and the existing teardown checks are required for a pass.

Five additional, isolated processes test Guest default termination, termination
after a Guest one-shot is consumed, default termination on a GNU pthread,
an ignored synchronous fault, and a Guest-blocked synchronous fault. The last
two follow the native Linux forced-fault behavior; they must not retry forever
or invoke a logically blocked handler. A standalone core native-libc control
also checks termination after ignoring a real trap.
The oracle requires `SIGTRAP` termination and matching pre-fault observations
with the expected thread identity. An arbitrary nonzero exit, including the
runtime's fail-closed exit codes, fails these tests. These death cases do not
claim successful runtime shutdown; the positive case must still close the module
and destroy the VM.

The native function is invoked through the real bridge trampoline. No Java
frames lie between the target checkpoint and the abandoned inner Guest call.
Nineteen signal-stack cases cover independent Guest configurations, actions
with/without `SA_ONSTACK`, saved/unsaved jump masks, native `setcontext`, nested
delivery, and AUTODISARM `swapcontext` suspension across ordinary Host callbacks
and a second fault. GNU pthreads also run the normal, nested and AUTODISARM cases.
Eight of the nineteen cases cross from a Guest handler to a Host handler which
abandons the Guest invocation. These eight transfers are reported separately
as `stack_unwinds`, in addition to the original 74 transfers. Checks require the
correct Host ordinary/alternate stack and persistence of a disabled AUTODISARM
stack; the fixture separately releases its own abandoned Guest resources.

Host signals 50/51 must remain pending when blocked and deliver to Host after
unblocking. Production does not use the standalone adoption fixture's synthetic
notification handlers or drain these Host signals on carrier release. A separate
core lifecycle test checks that carrier cleanup preserves application mask
changes and pending signals, without conflating them with VM detach behavior.

Nineteen additional wait cases check `sigpending`, `sigtimedwait`, `sigwaitinfo`,
`sigwait`, `sigsuspend`, `ppoll`, `pselect`, both epoll mask-wait APIs and
`signalfd`. Each adopted case must preserve Host's three queued realtime values,
sender metadata and FIFO order. Timeouts, Host interruptions, error paths and
NULL masks must restore the original masks. Native GNU pthread waiting and
cancellation are positive controls; both runners stage/hash the required GNU
`libgcc_s` cancellation dependency. `guest_wait_skipped` explicitly reports two
epoll cases if the environment returns `ENOSYS` for `epoll_pwait2`; zero means
none were skipped. This alone does not distinguish missing kernel support from
system-call policy restrictions.
Two additional `wait_unwinds` require observing `rt_sigsuspend` through the owned
carrier's `/proc/self/task/<tid>/syscall` before sending the escape signal.
They test saved/unsaved Host jumps, subsequent JNI calls and explicit fixture
sender cleanup, in addition to the 74 original and eight stack jumps.
The oracle requires individual wait/unwind observations, not just summary counts.
See [signal waiting](../../../core/docs/en/SIGNAL_WAITING.md) for the scope.

This fault fixture returns to a checkpoint in the same Guest invocation. See
the adopted-carrier tests below for retiring a suspended Host callback.
[The recovery contract](../../../core/docs/en/NONLOCAL_RECOVERY.md) also
covers normal JNI return, Java exception propagation and surviving Java frames.

## Host-blocked synchronous-fault acceptance

Add `--host-fault-mask` to `--native-unwind --nonlocal` to require successful
Guest trap recovery while the suspended Host blocks `SIGTRAP`. This writes a
separate `-host-mask` result directory. It is an acceptance test, not an expected
death case: `SIGTRAP` termination is a failure and never creates a passing summary.

The fixture explicitly enables the internal deferred-fault channel before VM
creation, reserving six unused Host RT signals. Guest execution leaves fault
signals physically unblocked and blocks private notifications. Host's logical
mask is unchanged. Async arrivals for a blocked Host fault signal retain their
original `siginfo_t` in a per-recipient queue; the notification is only a level
wakeup. A real Guest trap must reach Guest's handler and recover via its libc.

Thirteen Guest recoveries are required. Two asynchronous sends on each call must
coalesce, preserving the first payload, PID, UID and code. Host `sigpending`
exposes the original signal and hides notifications. One arrival is delivered
only after Host unblocks; three are consumed by `sigwaitinfo`, `sigtimedwait` and
`sigwait`. A subsequent zero-time wait returns `EAGAIN`. Context release with
pending work returns `EBUSY`, preserving the live runtime; after consumption,
release and another JNI call must succeed. Core separately tests thread exit
discarding only that recipient's pending work. Two further calls consume through
Host signalfd, using ordinary and vectored reads and a duplicated descriptor.
Both check original metadata, poll readiness before/after consumption, and a
subsequent JNI call. The `host-signalfd` JSON row is required by the oracle.

Host `fdopen/fread` additionally consumes a Guest-produced packet through a real
native FILE. The `host-signalfd-stdio` row requires original metadata, unchanged
fileno identity, native reopen/close and a subsequent JNI call. Core's native
[stdio comparison](../../../core/docs/en/STDIO.md) covers wide and partial reads
and distinguishes the GNU hidden-read fix from Bionic's already working path.
Four further calls exercise read/readv abandonment and fresh consumption after
recovery. A native Host handler jumps after kernel consumption and before
decoding, without calling JNI or crossing Java frames. Each recovery must allow
context release, a new JNI call and a fresh signal read. The oracle also requires
the `host-signalfd-unwind` row.
Three further Guest calls check that installing `SIG_IGN` and restoring the previous
Host action discards old pending work, permits context release and preserves a
fresh signal read and another JNI call. A Guest trap must still recover while
Host ignores that number; asynchronous Host arrivals are discarded. The
`host-ignore-pending` row is required.

This opt-in case passes on native Linux x86_64 and Android ARM64/JRE17. The
descriptor lifecycle and Guest delivery cases exercise the corresponding
V2 paths with the matching GNU service. See
[signal waiting](../../../core/docs/en/SIGNAL_WAITING.md).

Every nonlocal run also sends an asynchronous `SIGSEGV` from Guest to the
unblocked Host action. It must preserve sender metadata and execute with Host
TLS, rather than entering the synchronous Guest-fault path. The fixture chains
through HotSpot's native signal chain: its fallback is installed before VM
creation and HotSpot owns the outer action. The test verifies that the VM action
remains unchanged. JVM signal-checking warnings still fail the run.

An additional untagged `SIGUSR1` is sent while the carrier has Guest TLS. The
Host handler must update the originating Host thread's TLS and preserve sender
metadata; returning must retain the Guest TLS canary and errno. Registering this
Host handler must not change the exclusive reservation mask. This tests external
Host delivery on an adopted carrier, not tagged Guest delivery or GNU pthread
Host adoption.

## Independent Guest delivery on adopted carriers

Run the installed test-enabled SDK fixture with
`--native-unwind --adopted-signals`.
The fixture configures an explicit unused Guest notification number and
bootstraps the Host deferred-signal lanes. A GNU pthread sends each packet to
the adopted carrier and the test requires the original TID, Guest TP, native
stack placement and payload at the handler.

The cases cover a Java busy loop, blocked Host `ppoll` and `nanosleep`,
a Host-target jump, saved and unsaved Guest-target jumps through native
`GetVersion`, and nested Guest/Host/Guest delivery. The nested cases use
ordinary and alternate stacks and also abandon both signal invocations on a
jump back to the original Guest command. Every case calls JNI again before
module unload and SDK shutdown.

Three JNI result cases use a real tagged signal from a GNU pthread while
Host owns an unpublished primitive array critical pointer, a string critical
pointer, or a global reference. The handler on the adopted carrier restores a
Guest checkpoint with `siglongjmp` or `setcontext`. It performs no JNI calls
while the critical pointer is live. The Host releases the pending result
before the interrupted Guest JNI call is abandoned.

Guest `siglongjmp` and `setcontext` also resume an outer handler while retiring
the intervening Host callback and younger Guest activation. The `setcontext`
cases pass a copy from the stack that is about to retire, check the target
mask and floating-point rounding, and exercise a round trip with `swapcontext`
on a Guest stack prepared before the signal activation. The native return link
resumes the handler within that activation.

The cross-scope `swapcontext` case keeps the outer Guest handler on its normal
stack and the inner handler on an `SS_AUTODISARM` alternate stack. It switches
between them three times while the intervening Host callback is still live.
Three further Host callbacks run during the suspension. Assertions cover both
Guest stack frames, the Host callback's stack data, per-context floating-point
rounding and masks, rearming the alternate stack, another JNI call and teardown.
A separate case verifies `EINVAL` for an overlapping Guest stack and checks
that the inner handler and the Host callback still return normally.

Three cases leave an inner activation parked when the selected outer handler
returns or when a Host jump leaves it. One case parks two younger handlers on
separate alternate stacks, each with an intervening Host callback. After
retirement, a fresh Guest invocation rejects each stale context with
`ENOTSUP`. The fixture also checks that retirement is no longer busy and
verifies subsequent JNI use, module close and runtime shutdown.

A returning Guest handler edits the parked GNU context's FP control state.
The next native entry must observe the edit. The absolute `clock_nanosleep`
case checks that a Guest notification delivered during the kernel sleep leaves
the original Host deadline intact.

Linux x86_64 runs 21 cases, including the Host glibc context case. That case
resumes a same-scope `swapcontext` capture before restoring its outer
`getcontext` target. Android ARM64 runs 20 cases with the selected shell JRE17.
Guest glibc context cases run on both targets. The Host glibc case requires
public APIs absent from Bionic.

## HotSpot suspension and sampling

Run the installed SDK fixture with `--native-unwind --vm-signals` and the
selected HotSpot JRE17. A Java-created thread enters Guest JNI and parks two
frames through nested `swapcontext` calls on guarded stacks. JFR must report
a `jdk.NativeMethodSample` of that thread in Guest JNI. A Guest-to-Java
callback keeps the frames parked and must generate a `jdk.ExecutionSample` of
the callback. Each sample has to match the target thread, method and time
window. The test observes HotSpot's reserved suspend/resume signal routing.

While the target is back in Guest, HotSpot suspends the thread at its next Java
transition. Guest restores the parked stacks and releases their mappings, and
Java resumes only after the explicit VM resume. The test checks TID, TLS, errno,
stack canaries, FP rounding, signal masks, a later JNI call and shutdown.
Android runs the reviewed shell JRE17 build.

## Evidence

Each output directory contains commands and input hashes in `manifest.json`,
raw logs, and per-case JSON observations. A timeout, unexpected exit, malformed
observation or JNI/VM handler warning produces `failure.json`. `summary.json` is
written only after every selected case passes. Intermediate `OBSERVED` rows are
not success markers. Previous summary/failure files are removed at run start.
Manifests also bind Guest sources, installed core headers/stack assembly, libffi
and the imported Host archives used to build the consumer.

From the workspace root,
`python3 android-jni/tests/same_thread_results.py --require-current`
verifies input hashes for 23 selected desktop/Android suites
and writes `android-jni/build/tests/same-thread-compatibility.json`. Missing,
failed or stale evidence fails this check. Use `evidence_current` for the
result of this matrix. The report's `full_compatibility: INCOMPLETE` field
reflects its limited scope. The separate inline app fixture covers the reviewed
ART late profile and live registration reclamation. See
[VM adapter validation](../../../jvm-jni/docs/en/ADAPTER.md).

Output directories are under `android-jni/build/tests/`, with `-android`,
`-nonlocal` and `-recovery` suffixes identifying the selected fixture. They are
generated evidence; do not change manifests to make stale artifacts match.
