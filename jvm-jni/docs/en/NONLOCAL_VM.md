# Managed frames and native continuations

[English](../en/NONLOCAL_VM.md) | [简体中文](../zh/NONLOCAL_VM.md)

Native transfers that skip Java/VM frames are outside glibc-worker's recovery
contract. Restoring native registers does not perform Java stack cleanup,
including synchronization. The project restores valid native continuations
across libc domains and reconciles bridge-owned resources. It does not add Java
stack unwinding or require a custom JRE to support such transfers.

## Reproducible control

Run with an explicitly selected Linux JDK containing a compiler, headers and
`lib/server/libjvm.so`:

```sh
python3 jvm-jni/tests/nonlocal-vm/run.py --jdk /path/to/reviewed-jdk17
```

The reproducer does not load glibc-worker or a second libc. It embeds the
selected HotSpot and registers two ordinary native methods. Java enters a
monitor, calls native code, and has a `finally` block. Native code either
returns, throws a Java exception and returns, or transfers directly to a live
native checkpoint using `siglongjmp` or `setcontext`.

An independently attached waiter then attempts to acquire the same monitor.
Another attached observer checks its actual JVMTI thread state. Consequently,
the failure observation is `BLOCKED_ON_MONITOR_ENTER`, not an inference from a
timed-out sleep. Both interpreter and default execution run in separate child
processes. An abandoned VM is process-disposed; that is never reported as
successful VM destruction or bridge compatibility.

On the selected JDK17, ordinary return and Java exception controls run the
`finally`, release the monitor, let the waiter proceed and destroy the VM.
Direct native transfers reach their native targets but skip the `finally` and
leave the waiter blocked on the monitor. Structured results and hashes are
written under `jvm-jni/build/tests/nonlocal-vm/`. This establishes a managed
frame cleanup issue independent of the dual-libc bridge. This is boundary
evidence, not a bridge failure, a native-recovery blocker or proof of a HotSpot
defect. The same managed-frame reproducer has not been run with the bridge
loaded; the installed bridge tests exercise different, native-only transfers.

## Project responsibilities

Valid native cross-domain recovery remains a first-release requirement,
including nested transfers and bridge-owned JNI resource cleanup. An otherwise
valid native target must not be replaced by quarantine merely because it
crosses libc domains. The abandoned suffix must not contain Java/VM frames.
Java callers outside that suffix must remain usable after recovery.

Normal JNI callbacks, native returns, Java exception propagation, references,
monitor operations through JNI and VM thread state on supported paths remain
compatibility obligations. The bridge must not break these paths. A native
jump is not a Java exception; the bridge does not make one transparently execute
Java `finally` blocks or release monitors in skipped Java frames.

Destructive native jumps and resumable native contexts still need different
lifetime protocols. Excluding managed-frame abandonment does not remove the
required native `siglongjmp`/`setcontext` work or establish that it is complete.

HotSpot's [`JavaCallWrapper`](https://github.com/openjdk/jdk17u/blob/master/src/hotspot/share/runtime/javaCalls.cpp)
normally coordinates frame anchors, handles and thread transitions on return.
The [JNI exception contract](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html#exceptions-and-error-codes)
propagates pending Java exceptions when a native call returns. Neither source
supplies a generic cross-managed-frame `setcontext` recovery interface.
