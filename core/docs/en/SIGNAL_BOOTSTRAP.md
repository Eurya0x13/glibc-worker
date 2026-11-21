# Host signal bootstrap

[English](../en/SIGNAL_BOOTSTRAP.md) | [简体中文](../zh/SIGNAL_BOOTSTRAP.md)

Link `glibc_worker::signal_bootstrap` into the native Host executable. It
exports the signal registration entrypoints from that executable and adopts
the inherited signal actions before the Host starts a VM or Guest code. The
same API serves native GNU/Linux and Android hosts. It requires no ROM image
digest or import offsets.

For Android, the launcher can supervise the executable from its Activity and
start it as a fresh process with the app UID. Package the executable with the
app's native libraries. On the verified ARM64 configuration, the Activity
uses `/system/bin/linker64` with the installed executable path. HotSpot and the
glibc Guest share that child's address space. The Android UI uses ART in the
supervising process.

## Link and start

For a launcher using the HotSpot `System.load` adapter, use the installed CMake
packages and link the JNI layers statically into the same executable:

```cmake
find_package(GlibcWorkerJvmJni CONFIG REQUIRED)
target_link_libraries(native_host PRIVATE
  glibc_worker::signal_bootstrap glibc_worker::jvm_jni_static)
```

The static JVM target pulls in the static android-jni and bionic-dlopen
targets. For the public JNI bridge alone, use `glibc_worker::android_jni_static`.
Use a single SDK composition in the process so Guest admission, signal
registration and shutdown share one router.

1. Call `gw_host_signal_bootstrap()` from `signal_bootstrap.h` before loading
   the JVM or creating other application threads. The bootstrap checks that
   the Host's exported symbols own the registration paths. It returns
   `ENOTSUP` if ART, libsigchain or HotSpot is already loaded under its
   standard library name, or if registration interposition is unavailable.
2. Set up any explicitly selected Host notification channels through
   [`signal_channel.h`](SIGNAL_WAITING.md). Start the launcher-selected JVM.
3. Call `gw_host_signal_contract_init(&signals)` after channel setup and
   pass the resulting contract as `gw_host_runtime_config.signals` when
   starting the [JNI runtime](../../../android-jni/docs/en/RUNTIME.md) with
   the JVM's real `JNIEnv`.
4. Quiesce callers, close modules and shut down the JNI runtime. Destroy the
   JVM and quiesce signal recipients, then call `gw_host_signal_restore()` on
   the bootstrap thread. Keep runtime mappings for the process lifetime.

Startup and teardown calls return zero or an errno value. `EALREADY` reports
an active router. `EIO` requires the supervisor to dispose of the process.
During teardown, `EBUSY` permits retry after the outstanding owners quiesce.
Host code must register managed signals through the executable's exported
entrypoints. Raw registration syscalls and function pointers obtained directly
from a libc handle bypass this route.

## Launcher-owned JVM integration

**Launchers maintain their own JVM integration.** Select the Android JRE
artifact, preserve its required directory layout, create the JVM with the
appropriate invocation settings, and maintain the native loading route for
that artifact. When using jvm-jni for `System.load`, supply the reviewed
load/find/unload family and exact libjava/libjvm identities. These describe
the selected JRE's private loading interface. See the
[JVM adapter contract](../../../jvm-jni/docs/en/ADAPTER.md).

The [System.load fixture](../../../jvm-jni/docs/en/SYSTEM_LOAD.md) covers
Android app UID bootstrap, Bionic private registrations and same-thread JNI.
The core `signal_bootstrap` CTest covers inherited actions, a preloaded
foreign signal chain and restoration.
