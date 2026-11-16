# JNI failure handling

[English](../en/FAILURES.md) | [简体中文](../zh/FAILURES.md)

This contract covers valid attached threads using bridged JNI and Host entry into Guest natives. Failures preserve existing Java pending exceptions. If exception lookup/throw itself fails, retain the VM exception; no guarantee exists of allocating a specified exception during system-wide OOM.

## Error mapping

| Cause | JNI path | C loading API |
| --- | --- | --- |
| Reference/ID, class record, trampoline or thread-context allocation failure | Existing exception, otherwise OutOfMemoryError | GW_E_CAPACITY where an error output exists |
| Context/frame growth or index-overflow boundary | Existing exception, otherwise OutOfMemoryError | Command GW_E_CAPACITY |
| Long descriptor, invalid arguments, registration batch outside 0..64 | IllegalArgumentException | GW_E_ARGUMENT |
| Stale/wrong-kind token, closed module, unavailable bridge | IllegalStateException; quarantined native entry retains UnsatisfiedLinkError | BRIDGE, NOT_READY, QUARANTINED etc. |
| libffi preparation failure (not closure allocation) | UnsatisfiedLinkError on registration | GW_E_BRIDGE |

Existing exceptions take priority. Normal VM exceptions, NULL and numbers retain JNI semantics; not every NULL means exhaustion. Cache-capacity fallback to original JNI is not exceptional.

On bridge failure, status-returning JNI functions return JNI_ERR: Throw/ThrowNew, PushLocalFrame, EnsureLocalCapacity, RegisterNatives, UnregisterNatives, MonitorEnter/Exit and GetJavaVM. Other results use NULL/zero; callers must check exceptions. Host initializes return storage before failing. Failed argument tokenization never calls Guest or passes the failed reference onward as a valid NULL.

Guest AttachCurrentThread/AsDaemon always clears output JNIEnv on failure. Context/worker exhaustion returns JNI_ENOMEM; other VM attach errors retain their status. Unattached threads have no valid JNIEnv, so no invented Java exception conveys failure. Unattached/wrong JNIEnv use is invalid; RPC rejects with errno without guaranteeing a pending exception.

## Dynamic frames and emergency RPC

Normal Host commands/Guest RPC start at 12 depths and grow by 16; 12 is not an execution limit. An independent emergency RPC reports growth failure and permits exception query/clear, DeleteLocal/Global/WeakGlobalRef, string/array release and MonitorExit. Host cannot issue another Guest command during emergency processing, preventing recursive slot reuse.

If ExceptionOccurred cannot allocate its token, preserve the existing exception. ExceptionCheck/Clear do not allocate reference slots. VM/thread StackOverflowError remains intact; neither unlimited recursion nor recovery from native stack exhaustion is promised.

Private protocol v6 has stable dynamic directories, emergency RPC, per-context diagnostic addresses and owner lifecycle inbox. Rebuild/deploy Host and Guest together. Handshake checks structures, frames, initial capacities and page widths; v5 or older cannot mix. See [contexts and ownership](CONTEXTS_AND_OWNERSHIP.md).

## Rollback and ownership

If wrapping a newly created VM local/global/weak reference fails, delete that new reference; storage must not delete borrowed native arguments. ID insertion failure deletes its new weak class. Registration failure before VM entry removes newly created trampolines/unpublished records but retains old ones. After VM entry, partial publication is possible; retain resources until safe unregister. TLS binding/worker startup failure withdraws the new context reservation.

Uncertain unregister/cleanup uses [permanent quarantine](LOADER_BACKEND.md#卸载与失败隔离): retain possibly reachable resources and require process disposal. Ordinary exhaustion does not automatically quarantine. Failed explicit unregister of an owned class retains ownership and trampolines.

## Validation

`tests/failures.py` compiles production Host/Guest RPC under ASan/UBSan, injecting shared frame/context pages, private records, references/IDs, TLS and worker attach failures; it checks states, original exceptions, rollback and subsequent calls with VM/worker substitutes, not Android dual libc. `tests/bindings.py` uses real libffi closures to verify failed frames/arguments issue no command and balance registration/closure allocation failures. resources.py covers tokens/stores; audit.py covers load/unload transactions.

Android coverage uses real HotSpot CheckJNI for three rounds of 32-level reentry and 32 Host/Guest threads, growth, registration-count rejection and reuse. Results require zero bridge errors and final zero active references/closures. Mapping/emergency fault injection remains desktop production-path testing. This is not system-wide OOM, full ART validation or proof arbitrary modules recover partial operations.

## Lifecycle failures

Initialization failure after startup claim, handshake failure, hook restoration failure and shutdown join failure permanently quarantine. Wrong-thread/state or busy shutdown rejects before releasing resources. Synchronous calls have no universal timeout/cancel acknowledgement; a caller deadline never authorizes releasing borrowed arguments, references or images. See [lifecycle](LIFECYCLE.md).
