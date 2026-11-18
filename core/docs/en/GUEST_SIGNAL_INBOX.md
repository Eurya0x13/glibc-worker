# Guest signal inbox

[English](../en/GUEST_SIGNAL_INBOX.md) | [简体中文](../zh/GUEST_SIGNAL_INBOX.md)

The internal `signal-inbox.h` queue is the pending-state substrate for independent
Guest asynchronous delivery. It is compiled into `glibc_worker::domains`; the
installed internal source can also be compiled by the owning Guest libc. Do not
call Host-compiled queue functions while using a Guest thread pointer. Only data
crosses this boundary, not libc allocation ownership or function implementations.

The V2 JNI service uses this inbox for tagged Guest deliveries. The matching
GNU send hook supplies packets, the action table selects a disposition, and the
target's entry uses its Guest stack and TLS. An adopted carrier can handle a
packet while running Host or Java code. See
[Guest sends](GUEST_SIGNAL_SEND.md) and [asynchronous entry](GUEST_ASYNC_ENTRY.md).

## Selection and ownership

Each inbox belongs to one domain. It contains a process pending queue; each stable
receiver registration owns a separate thread pending queue and logical mask.
Host kernel pending queues do not participate in Guest selection.

Selection holds one inbox transaction across both queues. Thread pending wins
before process pending, even when a process-directed packet has a lower number.
Within either queue, the six fault numbers take priority, then the lowest number
wins. Asynchronous `SI_QUEUE` packets naming a fault obey that same priority.
This follows Linux [`next_signal`, `dequeue_signal` and `collect_signal`](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c).
Ordinary signals coalesce independently in each queue and retain the first
`siginfo_t`. Realtime signals preserve FIFO within each number and queue.

`gw_signal_inbox_deliver` samples the receiver mask under the selection writer;
even a stale notification cannot consume a blocked packet. Consuming waits use
`gw_signal_inbox_take` with an explicit eligible set that may include blocked
signals; the queue does not silently intersect that argument with the stored
mask. Pending queries include both queues irrespective of the logical mask.

Host-reserved numbers reject publication with `EBUSY`. Invalid, uncatchable,
glibc-private numbers 32/33 and positive kernel-origin `si_code` reject with
`EINVAL`. This internal operation is not a signal-zero existence probe, a public
user-pointer copy boundary or an implementation of process-group sends.

## Notification and lifetime

Every operation requires catchable signals physically blocked on the caller,
including libc-private cancellation signals. Use the raw mask guard; a virtual
Guest mask or a libc wrapper that filters private numbers is insufficient. The
inbox
writer serializes send, mask changes, selection, quota and recipient close. A
notification callback may issue only a nonblocking notification; it cannot
reenter the inbox or execute an application handler directly. Another recipient
may enter its notification handler immediately and wait for publication to
finish. A self-notification remains blocked until the caller releases the writer.

The callback receives the directed receiver or `NULL` for process pending. The
integration must select an eligible live Guest recipient for shared work, retain
work when all receivers block it, and reconcile pending work when admitting a
new receiver. Unblocking an existing receiver invokes notification for eligible
private or shared work. Registration alone does not expose a new thread to
asynchronous entry before the integration publishes its stack/TLS state.

Publication distinguishes rejection from failed wakeup:

| Result | Meaning |
| --- | --- |
| `status != 0` | Nothing was queued by this call. |
| `status == 0`, `wake_error == 0` | Packet accepted/coalesced; notification succeeded or was unnecessary. |
| `status == 0`, `wake_error != 0` | Packet remains accepted; notification needs retry or terminal handling. |

If retrying is possible, retry only the notification for the accepted packet.
Retrying the entire send could duplicate an RT packet. An earlier coalesced
standard packet retains its place after a callback failure.

Closing a receiver under the same writer prevents future sends/notifications and
releases its pending RT budget. Group close rejects new work and drops shared
pending; receivers then close their private queues. Destruction is separate and
requires sender, handler and saved-continuation quiescence. A zero live-receiver
count alone does not establish that boundary. Metadata is never reclaimed or
reused while a saved native continuation could refer to it. The integration's
stable registration owns target lifetime; a cached TID is not a lifetime token.

The configured RT limit is shared across all private and process queues of the
domain. It is a local resource budget, not per-UID Linux `RLIMIT_SIGPENDING`
accounting. Native notification allocation and its failures remain separate.
Standard packets do not consume the RT budget. Queue chunks grow as needed and
are retained until quiescent destruction; there is no fixed packet table.

## Validation

Run the native/Linux or Android ARM64 suite:

```sh
python3 core/tests/signal_deferred.py
python3 core/tests/signal_deferred.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

`signal_inbox_test` compares twelve private/shared queue scenarios against actual
kernel `rt_sigtimedwait`, including masks, coalescing, async fault priority, RT
FIFO, sender metadata and thread-before-process selection. Concurrent producers
and consumers check exactly-once selection; resource cases cover cross-receiver
quota, rejection, close and quiescent reuse. A controlled concurrent close
cannot revoke target lifetime inside the notification transaction. A GNU
asynchronous-cancellation case verifies publication finishes before restoring the
physical mask admits cancellation; Bionic has no `pthread_cancel` API. Its
notification test interrupts a
busy native Host thread without Guest-call or queue polling, verifies blocked
work is retained, retries failed wakeup and preserves the Host's own handler for
the logical signal number. That test uses one libc and does not establish Guest
entry/TLS or VM compatibility.

A separate fixture connects the Guest-compiled inbox to adopted TCBs and
guarded entry stacks. It covers timely interruption of native Host/Guest
code and Host sleep/ppoll recovery on Linux and Android ARM64. The installed
V2 cases also exercise Guest delivery during Java and native recovery.
See the [cross-libc entry fixture](GUEST_ASYNC_ENTRY.md) and
[installed SDK tests](../../../android-jni/docs/en/SDK_TESTS.md).

The optional `signal_inbox_test --notification-control` native-only reproducer
queues an empty private notification and admits it through `ppoll`'s temporary
mask. It reports `CONTROL_ONLY`: even with `SA_RESTART`, the Host wait returns
`EINTR` with no Guest packet. The suite records this in `native_controls`.
The installed service relies on the Host wait adapter to preserve deadlines
and expose Host-owned interruptions after Guest delivery. See
[wait recovery](SIGNAL_WAITING.md) and
[Linux poll restart handling](https://github.com/torvalds/linux/blob/v6.12/fs/select.c).
