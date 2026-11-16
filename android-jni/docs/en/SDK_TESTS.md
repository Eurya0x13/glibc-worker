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

## Native unwind validation

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

The additional `guest-fpe` observation executes a real floating-point divide
with `FE_DIVBYZERO` enabled, checks `FPE_FLTDIV`, Guest jump recovery and the
subsequent ordinary JNI call, then restores the original floating-point
environment. Desktop requires `PASS_REAL_HARDWARE`. AArch64 exception trap
enables are optional: if native `feenableexcept` cannot enable them, Android
records `UNSUPPORTED_HARDWARE` and `recovered:false`; it is not counted as a
successful FPE recovery. An unexpected signal, missing observation or failed
recovery still fails the suite.

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

Guest fault recovery returns to a checkpoint in the same Guest invocation; it
does not abandon a suspended Host callback. General continuation lifetime
accounting remains unfinished. Tagged asynchronous Guest signals, mask/wait
APIs beyond those tested, remaining lifecycle windows and complete architecture
state are not established.
Those remain required by [the recovery contract](../../../core/docs/en/NONLOCAL_RECOVERY.md).
Native transfers that skip Java/VM frames are outside that contract; normal
JNI return, Java exception propagation and surviving Java frames remain required.

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

This opt-in case passes on native Linux x86_64 and Android ARM64/JRE17. Ordinary
integrations do not enable it. Descriptor lifecycle/alias integration gaps,
concurrent ignore/reset semantics and general independent asynchronous Guest
delivery remain acceptance work. This case and the normal positive/death
matrix do not establish those remaining requirements; see
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
verifies input hashes for ten selected desktop/Android suites
and writes `android-jni/build/tests/same-thread-compatibility.json`. Missing,
failed or stale evidence fails this check. A current selected matrix still
reports full compatibility as `INCOMPLETE` while the acceptance work in
[todo.md](../../../todo.md) remains.

Output directories are under `android-jni/build/tests/`, with `-android`,
`-nonlocal` and `-recovery` suffixes identifying the selected fixture. They are
generated evidence; do not change manifests to make stale artifacts match.
