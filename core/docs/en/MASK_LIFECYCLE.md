# Guest logical-mask lifecycle

[English](../en/MASK_LIFECYCLE.md) | [简体中文](../zh/MASK_LIFECYCLE.md)

The matched libc supplies private mask binding, inheritance and observation for
the owning runtime. These interfaces are compiled into GNU libc, called only
with that libc's valid TCB, and negotiated through its exact private capability.
They are not a Bionic thread-state adapter or an application-facing ABI.

## Binding and inheritance

`__gw_domain_mask_bind(logical, physical, active, owned_bits)` binds stable,
thread-owned pointers. An active binding keeps the chosen physical bits from
`physical` while exposing their independent values from `logical`. Other bits
retain native behavior. Both adopted carriers and genuine GNU pthreads can
bind; a genuine GNU thread still owns its real TCB and needs no Bionic state.
Binding twice returns `EALREADY`. NULL pointers or ownership of kernel-only
and GNU-private cancellation/credential signals return `EINVAL`.

NPTL records a new thread's logical mask before clone. Normal inheritance uses
the parent's Guest view, including when its borrowed carrier has a different
physical mask. An explicit GNU attribute mask takes precedence. The real child
initially restores that Guest mask on its own kernel thread. It must not inherit
the parent's unrelated Bionic carrier view.

In the child lifecycle hook, `__gw_domain_mask_inherited(logical, physical)`
returns immutable creation snapshots before a child runtime binding modifies
the planned physical mask. It does not query the current mask. The initial GNU
root and adopted descriptors have no such native-child record and return
`ENOTSUP`, leaving outputs unchanged. The child hook still runs with catchable
signals blocked, before NPTL restores its startup mask and enters user code.
A binding there can prepare different physical bits for runtime-private wakes;
NPTL applies them at its original unmask point. Receiver setup must precede that
point. These snapshots alone do not implement pre-start recipient admission;
the [creation lifecycle](THREAD_LIFECYCLE.md) supplies its separate callbacks.

## Observing logical changes

Install one immutable optional observer at serialized bootstrap with
`__gw_domain_mask_hook(1, callback)`. Invalid version/NULL callback returns
`EINVAL`; replacement returns `EBUSY`. The observer receives the new logical
mask with all catchable signals physically blocked, before native unmasking.
It runs only for an active binding. Ordinary mask queries do not notify it.

The owning runtime can update target eligibility and issue a nonblocking wake
inside this window. The callback must retain its storage, preserve the native
critical mask, and avoid application code or reentering mask operations. Libc
preserves errno around it. Notification failure belongs to the runtime's
accepted-but-unnotified error state; it cannot turn a committed logical change
into an invented successful rollback. A failed native mask commit republishes
the prior logical value before attempting physical restoration.

Public `pthread_sigmask`/`sigprocmask`, native context mask operations and the
temporary begin/end of adapted mask-waits share this observer. Context register
restoration remains in the owning libc's assembly. Native saved/unsaved jump
mask semantics stay distinct. Runtime-owned direct logical-mask updates still
need their own queue publication transaction; the observer does not intercept
arbitrary stores, raw syscalls or own signal-handler bookkeeping.

## Validation

The matched-libc control in `tests/guest-async/mask-control.c` covers a real GNU
root binding, native child startup, bound child startup, default/explicit
attribute inheritance and C11 creation. It verifies immutable snapshots,
physical wake-bit preparation, query behavior, observer critical masks, errno,
get/set/swapcontext, saved/unsaved jumps and temporary/NULL `ppoll` masks.
`tests/guest_async.py` builds and runs it using the explicitly selected runtime
on Linux or Android ARM64. The control adds no JNI/VM or complete independent
Guest signal-delivery claim.

The cross-libc entry fixture additionally creates native GNU children from
actual adopted Guest TCBs. The parent logically blocks `SIGUSR1` while leaving
the carrier's physical bit unblocked. An ordinary child must start with the
Guest bit blocked; an explicit empty attribute must start with it unblocked.
The child has no mask binding, so its actual native kernel mask establishes
inheritance rather than a virtual query alone.

The recipe includes the NPTL descriptor and create-path changes in its input
hashes. See native
[thread initialization](https://github.com/bminor/glibc/blob/glibc-2.44/nptl/pthread_create.c),
[descriptor state](https://github.com/bminor/glibc/blob/glibc-2.44/nptl/descr.h)
and [mask operations](https://github.com/bminor/glibc/blob/glibc-2.44/nptl/pthread_sigmask.c).
