# Host signals received on GNU threads

[English](../en/HOST_SIGNAL_RELAY.md) | [简体中文](../zh/HOST_SIGNAL_RELAY.md)

A genuine GNU pthread has its own GNU TLS and no suspended Host continuation.
It does not need bionic thread state to execute Guest code. A process-global
Host signal action can nevertheless be selected on that thread. The explicit
relay policy runs that action on a dedicated, real Host pthread. It never
borrows another thread's TLS or claims the GNU recipient is a JVM thread.

## Delivery contract

- Synchronous Guest hardware faults still execute the Guest handler on the
  original GNU thread. Relaying does not change fault ownership.
- A routed Host asynchronous arrival publishes its immutable kernel-selected
  disposition, original `siginfo_t` and live kernel `ucontext` to the Host
  dispatcher. The GNU entry uses only lock-free atomics and raw syscalls.
- The original recipient waits with catchable signals blocked until the Host
  handler returns. Its signal frame and request therefore remain alive without
  per-signal allocation, bounded queues or copying architecture FP-state pointers.
- The Host handler sees Host TLS, `pthread_self()` and kernel TID. These identify
  the dispatcher, not the original recipient. The third argument describes the
  original GNU recipient; supported context edits reach its kernel sigreturn.
  The original `siginfo` preserves the sender, code and payload.
- Logical action masks, `SA_NODEFER`, one-shot selection and the immutable action
  generation are applied by the existing router. The source kernel disposition
  still determines source-syscall `SA_RESTART` behavior. The dispatcher initially
  has no alternate signal stack, so `SA_ONSTACK` falls back to its normal stack.
- Queued requests are serviced serially. Native Host signals that arrive while
  the dispatcher exposes a handler mask can still nest through normal routing.
  There is no global exclusion between Host handlers on different Host threads.

This is an explicit change of handler thread, not transparent original-thread
delivery. A handler must not wait for the blocked original recipient to make
progress. A native jump into a different OS thread's stack is invalid; relaying
does not legalize it. The dispatcher is not implicitly attached to ART/HotSpot,
and relaying does not make JNI or other non-async-signal-safe operations legal
inside a handler. VM suspension protocols must target real VM threads.

Linux's process/thread targeting and kernel-selected actions are described by
[`kernel/signal.c`](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c).
The compare-and-block wait uses
[`kernel/futex/waitwake.c`](https://github.com/torvalds/linux/blob/v6.12/kernel/futex/waitwake.c).

## Integration and lifetime

The installed internal header `signal-relay.h` exposes
`signal_host_relay_start()` and `signal_host_relay_stop()`. A pure C embedder
initializes/adopts the router, starts the relay on a Host lifecycle thread,
then admits GNU code. The JNI runtime startup API performs this setup before
starting the Guest service. Both execution backends use the same policy.

Before stop, quiesce external senders and GNU recipients. Stop returns `EBUSY`
while a request owns an original frame and `EDEADLK` when called by its own
dispatcher. Busy stop retains the working dispatcher; a later stop can retry.
Successful stop joins the real Host pthread. JNI shutdown checks pending work
before retiring the bridge and stops the dispatcher after Guest control stops.
The router cannot restore while the relay runs, and reclamation cannot release
queued immutable action tokens. Failed runtime initialization retains resources
under the existing process-disposal contract.

## Verification

`signal_relay_test` covers Host TLS/errno, original metadata, changed handler TID,
32 concurrent registered recipients, selected-action replacement, busy stop and
restore guards. These are Host unit-test threads marked in the registry, not a
claim of a second libc.

The installed JNI native-recovery fixture additionally creates eight real GNU
pthreads under the independently loaded GNU libc. They send 64 thread-directed
Host packets, including asynchronous `SIGTRAP`. Checks cover Host TLS, GNU
TLS/errno/pthread identity, original metadata, original context-mask edits,
subsequent JNI calls and normal runtime/VM shutdown. Its structured
`gnu-host-relay` row is required by the result validator. This does not establish
general tagged Guest asynchronous delivery or unreviewed ART compatibility.
