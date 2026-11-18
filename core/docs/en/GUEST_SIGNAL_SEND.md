# Guest libc send routing

[English](../en/GUEST_SIGNAL_SEND.md) | [简体中文](../zh/GUEST_SIGNAL_SEND.md)

The matching adoption libc provides the optional, versioned
`__gw_domain_signal_send_hook` bootstrap extension. It connects public sends to
an integration's independent Guest queues without replacing NPTL thread
descriptors or inventing a second implementation of `pthread_kill` validation.
The matching JNI service installs the hook when V2 startup enables Guest
notifications. See [runtime configuration](../../../android-jni/docs/en/RUNTIME.md).

## Registration and callback

Resolve the extension from the owning Guest libc. Its private C ABI is:

```c
typedef int (*sender_fn)(void *thread, int tid,
                         const siginfo_t *information, int *handled);
int __gw_domain_signal_send_hook(unsigned version, sender_fn sender);
```

Version 1 and a non-null callback register once. Invalid arguments return
`EINVAL`; another registration returns `EBUSY`. There is no unregister or
replacement operation: the callback and integration state must remain alive
until the runtime can no longer send or resume any saved continuation.
Registration belongs to bootstrap, before application threads use the service.
This additive extension negotiates its own version; it changes neither the
existing adoption capability value nor the shared transport layout. A service
requiring it must reject an older libc lacking this symbol.

The callback executes with Guest TLS and Guest `siginfo_t`. A non-null `thread`
is a borrowed owning-libc descriptor and `tid` is its live target TID. The sender
holds NPTL `exit_lock` across the callback and native fallback for a different
thread; the current thread needs no exit lock. A null thread and zero TID mean
this process's shared Guest pending queue. No descriptor pointer or TID alone
establishes a recipient's independent registration lifetime.

Return a positive error code and set `*handled = 1` for a routed request. Setting
it to zero preserves the native send. An active integration must explicitly
reject unknown logical Guest targets rather than accidentally dispatch them to
Host handlers. Owning libc physically blocks all signals before the callback;
keep them blocked across registry/queue locks and return with that mask intact.
Use nonblocking notification and do not recursively send through these public
interfaces. Libc restores internal errno before restoring the sender's mask,
so an unblocked self-signal can execute before the send API returns. A real
handler's own errno change remains visible, as in the native implementation;
callback-internal errno changes remain hidden.

Accepted queue publication and notification failure are different outcomes.
Do not return an "unsent" error after accepting a packet: retry notification or
quarantine the recipient according to integration policy. The fixture requires
notification success for its pinned live targets. The independent inbox keeps
its documented admission, close, RT quota and shared/private selection rules.

## Covered owning-libc sites

| Interface | Logical target and information | Error convention |
| --- | --- | --- |
| `pthread_kill`, `raise`, `gsignal` | Validated thread, `SI_TKILL` | pthread error number; raise/gsignal use `-1` and errno |
| `pthread_sigqueue` | Validated thread, `SI_QUEUE` and unchanged `sigval` | pthread error number |
| `sigqueue(getpid(), ...)` | Shared Guest queue, `SI_QUEUE` and unchanged `sigval` | `-1` and errno |
| `kill(getpid(), ...)` | Shared Guest queue, `SI_USER` | `-1` and errno |

All routed packets preserve the Guest sender's PID and real UID. Callback
internals cannot alter a pthread caller's errno; successful process sends also
retain it apart from a real handler's own changes. Thread signal zero probes the
integration's live recipient without enqueuing a packet. Process signal zero,
other PIDs, process groups, invalid numbers, NPTL's internal signals,
`SIGKILL` and `SIGSTOP` retain native paths. The internal abort raise path
remains native as well. The four shared [job-control signals](SIGNAL_WAITING.md#shared-linux-job-control)
also use their native delivery paths. Kernel sends use the kernel recipient
and process dispositions.

For a still-valid joinable descriptor after kernel exit, modern
`pthread_kill` keeps its upstream zero result and never invokes the hook.
`pthread_sigqueue` returns `ESRCH`. The extension strengthens the latter's active
routing path by holding `exit_lock` through selection and notification; the
inactive path retains the upstream implementation.

## Evidence

Run the [cross-libc fixture](GUEST_ASYNC_ENTRY.md#reproducible-native-validation)
with a runtime built from the current recipe. Its owner now sends the original
28 packets through public `pthread_sigqueue`, `pthread_kill`, `sigqueue` and
`kill`. The receiver checks queue source, code, sender and queued values. Each
adopted carrier additionally checks all six public self-send spellings return
only after execution (24 extra packets), plus invalid/internal numbers,
reservations and errno. A separate owning-libc process without hooks compares
handler-induced errno changes in all six interfaces; the tagged fixture checks
that internal errno restoration does not erase those real handler effects.

Two controlled genuine GNU thread exit races hold publication at the hook,
allow the target to reach its NPTL exit transaction, verify the target TID is
still alive, then release the sender. They exercise both `pthread_kill` and
`pthread_sigqueue`, including native fallback under the lock and valid
post-exit descriptor probes before joining.

The fixture schedules logical masks and acknowledges consumption before
changing its chosen shared recipient. Both this fixture and the installed V2
service use the [core target registry](SIGNAL_TARGETS.md) for lookup, admission,
selection and retirement. See
[installed SDK cases](../../../android-jni/docs/en/SDK_TESTS.md) for tested
JNI/VM delivery and native recovery paths.
