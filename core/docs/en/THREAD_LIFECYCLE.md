# GNU recipient creation lifecycle

[English](../en/THREAD_LIFECYCLE.md) | [简体中文](../zh/THREAD_LIFECYCLE.md)

The matched libc's optional `__gw_domain_thread_create_hook` connects recipient
allocation to actual NPTL creation. Register its version 1 configuration at
serialized bootstrap, before creating application threads. The configuration
contains its exact size and four required callbacks; libc copies it. Invalid
arguments return `EINVAL`; replacement returns `EBUSY`. There is no unregister.
Callbacks and their code must remain available through runtime retirement.

## Creation phases

1. `prepare(key, logical_mask, physical_mask, cookie_out)` runs before clone,
   after descriptor and inheritance initialization. Allocate receiver storage,
   reserve the identity and give the parent and prospective child separate
   lifetime holds. Returning a positive error prevents clone. Preparation must
   undo its own partial work; no completion or child callbacks follow failure.
   Native `pthread_create` still translates `ENOMEM` into `EAGAIN`.
2. `complete(cookie, tid, error)` runs exactly once after successful preparation,
   after native clone and affinity/scheduling setup. A positive TID is the raw
   successful clone result, even when subsequent setup fails. Zero means clone
   did not create a thread. Successful completion must enable the identity
   before `pthread_create` returns, even if the child has not run yet.
3. `enter(cookie)` runs in the actual child before its initial native unmasking.
   The prior `__gw_domain_thread_hook` entry runs first, so its outer kernel
   route can already locate the GNU TCB. Finish entry state and activate the
   same target; activation of the same TID is idempotent.
4. `leave(cookie)` runs in the child after user/TLS cleanup, before the prior
   lifecycle hook removes the outer route. Revoke sends, discard private
   pending work and release the child's hold. Normal return, `pthread_exit`
   and native cancellation use this path.

Child entry and exit can both precede parent completion. A detached child's
descriptor may already be freed or reused then. Completion must use its opaque
cookie, never dereference the descriptor key. NPTL stores the cookie before
clone and passes it directly to child callbacks; parent completion retains a
separate local copy. Native failed stopped-start setup skips both child
callbacks. Completion with error must therefore retire the reserved child hold
as well as the parent's hold. It must not expose the failed child as runnable.

All four callbacks run with catchable signals physically blocked; libc
preserves the errno at each callback boundary. This adds no whole-API errno
guarantee to `pthread_create`: a native clone fallback can change errno even
when creation succeeds. They must retain that critical mask and must
not run application code, make nonlocal transfers or wait on their recipient.
The control deliberately stalls selected callbacks to expose ordering races;
production callbacks must use nonblocking publication. This API supplies no
Bionic state to a GNU thread and changes no public pthread descriptor rules.

## Core prepared targets

`gw_signal_target_prepare` reserves a stable receiver and key with TID zero
and one creator pin. Derive the prospective child hold with `retain`. Prepared
targets never consume or receive notifications, including shared admission,
mask changes and notification retries. Shared packets stay pending.

`gw_signal_target_activate(target, tid)` requires a positive TID and allocates
nothing. It admits delivery and checks previously pending work atomically.
Repeating the same TID succeeds without another initial notification; a
different TID returns `EALREADY`. Close wins with `ESRCH`, including when an old
descriptor key has a replacement identity. Initial wake failure retains the
committed admission and appears in `gw_signal_targets_error`.

The ordinary `open` operation remains one registry transaction, without an
observable prepared phase. Close/release retain the existing continuation
contract. The owner must hold the target across activation; a creator pin does
not prove saved signal frames or TCB storage can be reclaimed.

## Validation and scope

`tests/guest-async/create-control.c` connects the owning libc callbacks to real
prepared core targets. It checks 130 children, both parent/child orders, 64
detached early exits, native cancellation cleanup, signal publication before
child admission, explicit `pthread_exit`, TSD destructors, copied configuration,
inherited snapshots and callback-boundary errno. Separate
failure cases reject preparation, fail affinity after successful clone and
force clone failure with a thread-local seccomp filter. Failed setup must not
enter user code or child lifecycle callbacks, and every target hold must retire.
The disposable `--child-only` control deliberately omits parent admission and
must reproduce `ESRCH` for a still-live child paused before its entry admission.
The complete path must accept the same send and consume its original packet.
`tests/guest_async.py` runs this with the selected libc on Linux or Android
ARM64. The registry test also races activation, preserves shared packets while
prepared, rejects late activation after key reuse and retries failed wakes.

These controls establish creation ordering and storage lifetime. The installed
JNI service still needs to connect these callbacks to public independent Guest
dispositions and asynchronous entry; the control is not that compatibility pass.
Native descriptor ownership and failed startup are described in
[NPTL creation](https://github.com/bminor/glibc/blob/glibc-2.44/nptl/pthread_create.c).
