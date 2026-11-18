# glibc-worker android-jni

[English](../en/README.md) | [简体中文](../zh/README.md)

A bridge using public JNI APIs: real Host VM reference ownership, exceptions, JNI proxies, thread pairing, callbacks/reentry, libffi trampolines and JNI loading lifecycle. Private JRE hooks are outside this project.

`src/host/`, `src/guest/`, `src/common/` and `src/java/` contain Host, GNU service, shared internal protocols and Java helpers; `include/` contains public interfaces.

Host and Guest link their own bionic/GNU libc and libffi. The runtime API defaults to independent stacks/TCBs on the same carrier thread; worker execution is explicit. Low-level APIs also allow caller-owned worker bootstrap. Guest JavaVM/JNIEnv are bridge interfaces. The VM-neutral Java helpers provide bridge types, image policy and glibc library staging. See [runtime integration](RUNTIME.md) for startup, artifact handshake and signals.

## Standalone build

After installing core and bionic-dlopen, run from this project:

```sh
./build.sh -DCMAKE_PREFIX_PATH="/path/to/loader/install;/path/to/core/install" \
  -DGW_JNI_INCLUDE_DIR=/path/to/neutral-jni-header \
  -DGW_FFI_INCLUDE_DIR=/path/to/bionic-libffi/include \
  -DGW_FFI_LIBRARY=/path/to/bionic-libffi/lib/libffi.a \
  -DGW_FFI_LICENSE_FILE=/path/to/bionic-libffi/share/licenses/libffi/LICENSE \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
```

Android also needs NDK toolchain options. The JDK builds Java 8 helper JARs only. Outputs include `libgw_android_jni.so/.a`, `android-jni-helpers.jar` and `GlibcWorkerAndroidJni`, exporting `glibc_worker::android_jni`.

`build-guest.sh` separately builds the GNU side from explicit environment paths for the GNU compiler/libffi, installed core/loader headers and JNI headers. Core owns libffi builds and `core/build/ffi-{android,linux}/<ABI>/`; this project consumes explicit paths. Workspace composition supplies those inputs.

## Optional VM adapter

`include/jni_vm_adapter.h` defines install/restore hooks registered before bridge initialization. They are invoked only for `GW_HOST_VM_ADAPTER`; requesting it without registration fails. The old JRE17 naming alias was removed. Private implementation and pinned profiles belong to jvm-jni.

Existing protocols, exports and public JNI loader ABI are retained. The historical `jre_library_path` field in host configuration v2 is forwarded only to the optional adapter; this project does not infer private JRE layouts from it.

## Tests and boundaries

`tests/native/` and `tests/java/` own JNI, reference, exception, thread, reentry, SQLite and array fixtures. Real VM integration hosts compose them without copying them into jvm-jni. `tests/check-boundary.py` checks built DSOs/JARs for accidental private JRE implementation.

Real coverage uses Android HotSpot. Public JNI usage does not prove complete ART compatibility. See [loading](LOADER_BACKEND.md) and [array-copy caching](ARRAY_COPY_CACHE.md).

Bindings, reference/ID slots and registered-class records use [Zig stores](ZIG_BINDINGS.md), requiring Zig 0.16.0 to build. Initial capacities are 512, 16,384 and 256, growing in stable 64-slot blocks. The fixed C backend is removed; module capacity remains 64. C thin layers retain public ABI and libffi/JNI execution. Deploy matching Host/Guest artifacts together.

See [contexts and ownership](CONTEXTS_AND_OWNERSHIP.md) for thread binding, dynamic contexts/reentry, class handoff and dependency function attribution.

One runtime per process binds one selected VM. It cannot restart after shutdown or quarantine. See [lifecycle and process disposal](LIFECYCLE.md) for owner-thread rules, shutdown order, resource retention after timeout and supervision duties.
