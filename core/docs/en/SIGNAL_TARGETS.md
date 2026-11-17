# Guest signal target registry

[English](../en/SIGNAL_TARGETS.md) | [简体中文](../zh/SIGNAL_TARGETS.md)

`signal-targets.h` connects an independent inbox to real recipient admission,
logical masks, notifications and retirement. Compile its installed C source with
the owning libc, as for [signal inboxes](GUEST_SIGNAL_INBOX.md). Calling a
Host-compiled implementation while Guest TLS is installed is invalid.

## Identity and publication

Each target owns a stable receiver, opaque owning-libc thread key, kernel TID
and integration context. An open-addressed index handles directed sends; a
dense active vector supplies eligible process recipients. Both grow with checked
arithmetic. Closing removes active membership without moving the stable target.
A reused libc descriptor address creates a new target identity. Old pinned
receivers cannot consume the new target's packets.

The registry writer encloses inbox transactions, including the nonblocking
notification callback. Physically block catchable signals before every call;
the owning-libc send hook already establishes that condition. Callbacks must
not call the registry, allocate or wait for a recipient. Publication and close
therefore have one order: after close returns, no later send can notify that
target. A TID mismatch returns `ESRCH`; a thread signal-zero probe enqueues
nothing. A directed key must originate from libc's validated descriptor path,
not from an unprotected cached `pthread_t`.

Admit a real target only after its TCB is bound, entry stack/state is valid and
the outer kernel route can find it. Admission checks already-pending shared
work. Shared sends choose a currently eligible active recipient; if every
recipient blocks the packet, it remains pending. Unblocking a target rechecks
both private and shared pending state. Selection retains the inbox's Linux
private-before-shared and per-number ordering rules.

`gw_signal_targets_send` and `gw_signal_target_mask` return separate `status`
and `wake_error` fields. A nonzero status means no send/mask mutation happened.
A wake error means publication or the mask change already committed; do not
retry it as a new publication. Query `gw_signal_targets_error`, then retry
notification with `gw_signal_targets_retry`, or quarantine at the integration
boundary. Successful target admission also remains committed if its initial
notification fails; the registry error exposes that failure.

## Entry and saved-frame lifetime

`gw_signal_target_pin` admits a new entry only while the target is open.
An entry owning a pin can derive a saved-continuation hold through
`gw_signal_target_retain`, including after new delivery has been revoked.
`gw_signal_target_unpin` releases either kind of hold. The caller must track
every saved continuation and retired nonlocal suffix; the registry cannot infer
that a zero handler count rules out future `setcontext` resumption.

Close discards that target's pending private queue and revokes new sends/entries.
It preserves storage for existing pins. Release requires closure, zero pins and
the owner's explicit source/frame quiescence; otherwise it returns `EBUSY`.
The final registry destroy requires close and release of every retained target.
Neither close nor a failed release frees a still-owned TCB, stack or module.

The current cross-libc fixture opens recipients after actual TCB bind and entry
setup, pins every returning Guest entry and releases only after physical helper
drain, Guest finish and carrier join. It uses the registry directly from the
public libc send callback. Its small fixed carrier array now controls only the
test oracle's logical-mask schedule, not runtime target lookup.

## Verification and remaining integration

The native control grows to 2,048 stable targets, replaces a descriptor key while
an older identity remains pinned, races four senders against close, checks
shared admission/unblocking and injects accepted-but-failed notifications for
both publication and mask changes. It runs under the matched GNU libc on Linux
and Android ARM64. The Host/core variant also runs under Clang UBSan.

These are recipient/queue lifetime guarantees. The installed JNI service still
needs public asynchronous dispositions, complete logical mask/wait hooks and
general saved-frame coordination. The normally returning entry test does not
make arbitrary asynchronous nonlocal exits or VM integration complete.
