# Host stdio and deferred signal descriptors

[English](../en/STDIO.md) | [简体中文](../zh/STDIO.md)

The explicit deferred fault channel uses private kernel notifications to retain
original Host signal metadata. A Host `FILE` reading that signalfd must translate
the notification before bytes enter its native buffer. This applies to signal
descriptors only; ordinary Host/Guest file I/O and JNI arrays are unrelated.

The adapter keeps the owning libc's actual FILE object, lock, buffering,
orientation, seek/reopen behavior and descriptor ownership. There are no cookie
streams, extra descriptors, replacement FILE allocations or per-stream records.
Normal `fileno`, unlocked byte I/O, wide I/O, `freopen` and `fclose` retain the
native implementation. Native errors, including a too-small unbuffered signalfd
read and nonblocking `EAGAIN`, remain native results.

## Host backends

| Host | Integration |
| --- | --- |
| Android bionic LP64 | Native stdio already reaches the public read adapter on the validated artifact. Before channel activation, a private pipe/fread probe temporarily observes our cached read backend and verifies that path. The cache is restored before admission. No private FILE fields, factory replacements, libc data writes or permanent probe branches are required. |
| Linux GNU libc LP64 | Native FILE reads can bypass public read. Validate the exported byte/wide read-table sizes, original read callbacks, common libc ownership and fully protected GNU RELRO pages. Replace only those existing read callbacks at the serialized bootstrap boundary. The native read implementation still performs actual I/O and retains its cancellation and FILE bookkeeping. |

GNU vtable identity and validation remain intact; no foreign table is admitted
and no instruction bytes change. Data pages temporarily become writable and
return to read-only before application execution resumes. Layout mismatch fails
with `ENOTSUP`; denied writes return the native error. A failed rollback is
terminal, reports `EIO`, pins all callbacks and requires the embedding Host to
discard the process. The component does not kill the application automatically.

Both profiles handle pre-existing native FILE objects through their usual read
backend. Native hidden read paths outside FILE, raw syscalls and integrations
that bypass the explicit bootstrap contract remain outside this adapter.
The profile query `signal_stdio_profile()` identifies the active backend; it
does not certify arbitrary Host libc or ART artifacts.

## Lifetime

`signal_router_defer_faults()` activates the adapter after checking the requested
private signal numbers and descriptor inventory. Host stdio users and descriptor
owners must be quiescent during activation and restore, before creating the VM
or admitting producers. Failure before activation must not admit virtual signal
descriptors. Normal SDK startup does not enable this deferred channel implicitly.

`signal_router_restore()` first requires all virtual descriptors and deferred
recipients to retire, then restores GNU native read callbacks. Bionic needs no
callback/table restoration: the verified path is its ordinary native stdio path.
The temporary probe adds no branch to normal reads after activation. Existing
native consuming-operation rules still apply; this feature does not establish
general saved/resumable context recovery or native/deferred dequeue priority.

## Verification

Run the maintained native comparison fixture through the cross-platform suite:

```sh
python3 core/tests/signal_deferred.py
python3 core/tests/signal_deferred.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

`signal_stdio_test` compares complete metadata against a raw native signalfd,
then checks partial/unlocked reads, `EAGAIN` and `clearerr`, unbuffered errors,
wide orientation, real fileno/reopen/close behavior, ordinary files before/after
restore and 16 concurrent recipient-local streams.

The behavioral fixture can also link to the preceding `c034346` core SDK. On
Linux GNU libc 2.41, its virtual read exposes a private notification and fails
the original-signal metadata assertion. On the tested Android bionic artifact,
the same preceding SDK already passes; this is evidence to preserve its native
path, not a newly fixed Android bug. Weak profile queries let that comparison
run without weakening the observable metadata/FILE assertions.

The installed JNI `--host-fault-mask` fixture additionally produces the packet
while real Guest code executes, consumes it with Host `fdopen/fread`, validates
original metadata, performs another JNI call, reopens the native FILE and checks
descriptor closure. Its `host-signalfd-stdio` result is required by the oracle.

Relevant implementation sources:

- [GNU FILE read callbacks](https://github.com/bminor/glibc/blob/glibc-2.41/libio/fileops.c)
- [GNU wide FILE reads](https://github.com/bminor/glibc/blob/glibc-2.41/libio/wfileops.c)
- [GNU read table ABI](https://github.com/bminor/glibc/blob/glibc-2.41/libio/libioP.h)
- [GNU RELRO protection](https://github.com/bminor/glibc/blob/glibc-2.41/elf/dl-reloc.c)
- [Bionic native stdio](https://github.com/aosp-mirror/platform_bionic/blob/main/libc/stdio/stdio.cpp)
