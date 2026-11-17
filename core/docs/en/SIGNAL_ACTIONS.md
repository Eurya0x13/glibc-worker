# Guest signal dispositions

[English](../en/SIGNAL_ACTIONS.md) | [简体中文](../zh/SIGNAL_ACTIONS.md)

`signal-actions.h` joins logical dispositions to the
[target registry](SIGNAL_TARGETS.md). Compile the installed C sources with the
owning libc. Physically block catchable signals before every operation; the
writer order is dispositions, targets, inbox. Invoke application handlers only
after releasing every writer.

## Selection and replacement

The table has one entry for each Linux LP64 signal number. Each successful
registration receives a checked, monotonically increasing revision. Exhaustion
returns `EOVERFLOW` without changing the previous action. Invalid numbers return
`EINVAL`; configured registration reservations return `EBUSY`.

Selection copies the complete action and revision into caller-owned storage.
`SA_RESETHAND` changes the current handler to `SIG_DFL` in that same transaction,
retaining its flags and mask. Concurrent recipients cannot both select the
one-shot handler. Replacing a disposition does not invalidate a previously
selected snapshot and allocates no retired registration record.

A saved native continuation owns the lifetime of its snapshot storage. This
table neither proves that a suspended signal frame is dead nor pins application
handler code against an invalid native module unload. Target, stack and module
lifetimes still require their own ownership rules.

## Sending and ignoring

`change` flushes a newly ignored number from every private and shared queue
before publishing the action. This includes default-ignored `SIGCHLD`, `SIGURG`,
`SIGWINCH` and `SIGCONT`. Future sends, action changes and delivery selection
share the outer disposition transaction, so a sender cannot repopulate an
ignored queue between flush and publication. This follows Linux's
[ignored-disposition cleanup](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c).

An ignored send still validates its target identity and asynchronous reservation.
Linux retains it when the target blocks the number, including a temporary
wait's saved `real_blocked` mask. Only an unblocked ignored send avoids quota. A dead directed target returns `ESRCH`; ignoring a
reserved number does not bypass `EBUSY`. Registration and asynchronous-send
reservations may differ, allowing executing-domain hardware faults to coexist
with a Host-owned asynchronous channel.

`gw_signal_actions_representative` explicitly pins the logical group leader
before process sends. Its mask controls Linux's process-send ignore filter; a
later eligible recipient does not substitute for the group leader. Its identity
remains pinned after thread exit until the action owner closes. Unconfigured
process sends return `ENOTSUP`. `gw_signal_target_masks` publishes effective and
saved wait masks under the registry writer.

`deliver` selects the packet and its action together. The target registry's
thread-before-shared ordering and realtime quota accounting remain unchanged.
Accepted publication and notification failure retain separate result fields.
A stale kernel notification after a flush must take the hidden-notification
path rather than inventing an application delivery.

## Integration and validation

The opt-in JNI Guest uses action snapshots for its six synchronous fault
dispositions. Initializing with a null target registry supports this case
without logical asynchronous queues. Public asynchronous Guest service wiring,
kernel stop/continue job-control effects and general saved-frame recovery remain
unfinished; this table does not establish their compatibility.

`core/tests/signal_actions.c` covers snapshot replacement, aliased registration
arguments, one-shot races, ignore flush, quota recovery, reservations, revision
exhaustion, blocked ignored sends, representative lifetime and four concurrent
senders racing registration changes. A separate native-libc control in the same
test reproduces ignored-but-blocked pending and consuming-wait behavior for
standard, realtime and default-ignored signal numbers. It runs
under native Host/core CTest and Clang UBSan, and as an owning-GNU-libc control
through `core/tests/guest_async.py` on Linux and Android ARM64.

The filtered send also cancels opposing pending stop/continue signals across
all queues before ignore filtering, matching Linux generation-time ordering.
Native controls verify both directions. Kernel process-stop and process-resume
effects are separate unfinished integration obligations.
