#include "adapter_internal.h"
/* JNI trampolines that replace libjava's NativeLibraries native methods.
 * Each proxy must keep the exact HotSpot signature of its family because
 * RegisterNatives binds by name and descriptor. GW_PASS from the backend
 * means "not a Guest library": the original JVM path then runs. GW_OK and
 * GW_ERROR both skip it; the backend owns the outcome, including JNI_OnLoad. */
/* The saved libjava export for hook slot i; slots are the same in every
 * family: 0 = load, 1 = unload, 2 = find. */
#define ORIGINAL(i, type) ((type)gw_jvm.hooks[i].original.fnPtr)

typedef void (*load8_fn)(JNIEnv *, jobject, jstring, jboolean);
typedef jboolean (*load10_fn)(JNIEnv *, jobject, jstring, jboolean);
typedef jboolean (*load10_throw_fn)(JNIEnv *, jobject, jstring, jboolean,
                                    jboolean);
typedef jboolean (*load15_fn)(JNIEnv *, jclass, jobject, jstring, jboolean,
                              jboolean);
typedef jboolean (*load18_fn)(JNIEnv *, jclass, jobject, jstring, jboolean,
                              jboolean, jboolean);
typedef void (*unload8_fn)(JNIEnv *, jobject, jstring, jboolean);
typedef void (*unload10_fn)(JNIEnv *, jclass, jstring, jboolean, jlong);
typedef void (*unload15_fn)(JNIEnv *, jclass, jstring, jboolean, jboolean,
                            jlong);
typedef jlong (*find8_fn)(JNIEnv *, jobject, jstring);
typedef jlong (*find15_fn)(JNIEnv *, jclass, jobject, jstring);
typedef jlong (*find19_fn)(JNIEnv *, jclass, jlong, jstring);

/* JDK 8-9 void load. Builtin images belong to the VM and never reach the
 * backend. A backend failure has already thrown (throw_if_fail = 1), so
 * returning without the original call surfaces that exception. */
static void load8(JNIEnv *e, jobject lib, jstring name, jboolean builtin) {
  if (builtin || gw_jvm_open(e, lib, name, 1) == GW_PASS) {
    ORIGINAL(0, load8_fn)(e, lib, name, builtin);
  }
}

/* 10-14 GA load0. On the Guest path, true means the load committed without
 * the original; false carries the exception gw_jvm_open raised. */
static jboolean load10(JNIEnv *e, jobject lib, jstring name, jboolean builtin) {
  int rc = builtin ? GW_PASS : gw_jvm_open(e, lib, name, 1);
  return rc == GW_PASS ? ORIGINAL(0, load10_fn)(e, lib, name, builtin)
                       : rc == GW_OK;
}

/* 11u backport of load0. throwExceptionIfFail only governs loader-not-found;
 * JNI_OnLoad and version rejections still throw (see gw_jvm_open). */
static jboolean load10_throw(JNIEnv *e, jobject lib, jstring name,
                             jboolean builtin, jboolean throws) {
  int rc = builtin ? GW_PASS : gw_jvm_open(e, lib, name, throws);
  return rc == GW_PASS
             ? ORIGINAL(0, load10_throw_fn)(e, lib, name, builtin, throws)
             : rc == GW_OK;
}

/* 15-17 GA load. isJNI = false marks a load without JNI_OnLoad semantics;
 * such loads and builtin images must stay on the original path. */
static jboolean load15(JNIEnv *e, jclass cls, jobject lib, jstring name,
                       jboolean builtin, jboolean jni) {
  int rc = builtin || !jni ? GW_PASS : gw_jvm_open(e, lib, name, 1);
  return rc == GW_PASS ? ORIGINAL(0, load15_fn)(e, cls, lib, name, builtin, jni)
                       : rc == GW_OK;
}

/* 18 GA / 17u backport: the load15 filter plus throwExceptionIfFail,
 * forwarded as the loader-not-found policy. */
static jboolean load18(JNIEnv *e, jclass cls, jobject lib, jstring name,
                       jboolean builtin, jboolean jni, jboolean throws) {
  int rc = builtin || !jni ? GW_PASS : gw_jvm_open(e, lib, name, throws);
  return rc == GW_PASS
             ? ORIGINAL(0, load18_fn)(e, cls, lib, name, builtin, jni, throws)
             : rc == GW_OK;
}

/* 19+ load has the same two-boolean descriptor as 15-17, but the second
 * boolean is throwExceptionIfFail, not isJNI. It is therefore forwarded as
 * the throw policy, and the original runs through the positional load15
 * prototype that this descriptor still matches. */
static jboolean load19(JNIEnv *e, jclass cls, jobject lib, jstring name,
                       jboolean builtin, jboolean throws) {
  int rc = builtin ? GW_PASS : gw_jvm_open(e, lib, name, throws);
  return rc == GW_PASS
             ? ORIGINAL(0, load15_fn)(e, cls, lib, name, builtin, throws)
             : rc == GW_OK;
}

/* The 8-9 unload has no handle argument; the Guest handle comes from the
 * instance field resolved at install. Only GW_PASS (not a Guest handle)
 * reaches the original unload. */
static void unload8(JNIEnv *e, jobject lib, jstring name, jboolean builtin) {
  uint64_t id = (uint64_t)(*e)->GetLongField(e, lib, gw_jvm.handle);
  if (gw_jvm_close(e, id) == GW_PASS) {
    ORIGINAL(1, unload8_fn)(e, lib, name, builtin);
  }
}

/* Static unload receives the handle directly. This (String, boolean, long)
 * form is shared by the 10 families and by 19+, which dropped isJNI again. */
static void unload10(JNIEnv *e, jclass cls, jstring name, jboolean builtin,
                     jlong id) {
  if (gw_jvm_close(e, (uint64_t)id) == GW_PASS) {
    ORIGINAL(1, unload10_fn)(e, cls, name, builtin, id);
  }
}

static void unload15(JNIEnv *e, jclass cls, jstring name, jboolean builtin,
                     jboolean jni, jlong id) {
  if (gw_jvm_close(e, (uint64_t)id) == GW_PASS) {
    ORIGINAL(1, unload15_fn)(e, cls, name, builtin, jni, id);
  }
}

/* owns_handle also recognizes retired Guest tags, so a closed Guest library
 * raises a stale-handle error in gw_jvm_find instead of reaching the VM's
 * dlsym with a dead id. Non-Guest handles keep the original lookup. */
static jlong find8(JNIEnv *e, jobject lib, jstring name) {
  uint64_t id = (uint64_t)(*e)->GetLongField(e, lib, gw_jvm.handle);
  return gw_jvm.backend->owns_handle(id) ? gw_jvm_find(e, id, name)
                                         : ORIGINAL(2, find8_fn)(e, lib, name);
}

static jlong find15(JNIEnv *e, jclass cls, jobject lib, jstring name) {
  uint64_t id = (uint64_t)(*e)->GetLongField(e, lib, gw_jvm.handle);
  return gw_jvm.backend->owns_handle(id)
             ? gw_jvm_find(e, id, name)
             : ORIGINAL(2, find15_fn)(e, cls, lib, name);
}

/* 19+ findEntry0 lives on NativeLibrary and passes the bare handle; there
 * is no library object, so signature lookup relies on the remembered owner. */
static jlong find19(JNIEnv *e, jclass cls, jlong id, jstring name) {
  return gw_jvm.backend->owns_handle((uint64_t)id)
             ? gw_jvm_find(e, (uint64_t)id, name)
             : ORIGINAL(2, find19_fn)(e, cls, id, name);
}

#define OLD "java/lang/ClassLoader$NativeLibrary"
#define OLD_SYMBOL "Java_java_lang_ClassLoader_00024NativeLibrary_"
#define NEW "jdk/internal/loader/NativeLibraries"
#define NEW_IMPL NEW "$NativeLibraryImpl"
#define NEW_SYMBOL "Java_jdk_internal_loader_NativeLibraries_"
#define LOAD_PREFIX "(L" NEW_IMPL ";Ljava/lang/String;"
#define OLD_UNLOAD                                                       \
  {OLD, "unload",        "(Ljava/lang/String;ZJ)V", OLD_SYMBOL "unload", \
   1,   (void *)unload10}
#define OLD_FIND                                                       \
  {OLD, "findEntry",  "(Ljava/lang/String;)J", OLD_SYMBOL "findEntry", \
   0,   (void *)find8}
#define NEW_UNLOAD                                                        \
  {NEW, "unload",        "(Ljava/lang/String;ZZJ)V", NEW_SYMBOL "unload", \
   1,   (void *)unload15}
#define NEW_FIND                          \
  {NEW,                                   \
   "findEntry0",                          \
   "(L" NEW_IMPL ";Ljava/lang/String;)J", \
   NEW_SYMBOL "findEntry0",               \
   1,                                     \
   (void *)find15}

/* One row per gw_jvm_loader_family, in enum order: gw_jvm_shape indexes by
 * family - 1. Each row names the class owning the handle, jniVersion and
 * fromClass fields. has_loaded_field marks the 8-9 layout, where success is
 * signalled by setting `loaded`. Names, descriptors and symbols must match
 * the reviewed libjava exactly. */
static const struct gw_jvm_shape shapes[] = {
    {OLD,
     1,
     {{OLD, "load", "(Ljava/lang/String;Z)V", OLD_SYMBOL "load", 0,
       (void *)load8},
      {OLD, "unload", "(Ljava/lang/String;Z)V", OLD_SYMBOL "unload", 0,
       (void *)unload8},
      {OLD, "find", "(Ljava/lang/String;)J", OLD_SYMBOL "find", 0,
       (void *)find8}}},
    {OLD,
     0,
     {{OLD, "load0", "(Ljava/lang/String;Z)Z", OLD_SYMBOL "load0", 0,
       (void *)load10},
      OLD_UNLOAD,
      OLD_FIND}},
    {OLD,
     0,
     {{OLD, "load0", "(Ljava/lang/String;ZZ)Z", OLD_SYMBOL "load0", 0,
       (void *)load10_throw},
      OLD_UNLOAD,
      OLD_FIND}},
    {NEW_IMPL,
     0,
     {{NEW, "load", LOAD_PREFIX "ZZ)Z", NEW_SYMBOL "load", 1, (void *)load15},
      NEW_UNLOAD,
      NEW_FIND}},
    {NEW_IMPL,
     0,
     {{NEW, "load", LOAD_PREFIX "ZZZ)Z", NEW_SYMBOL "load", 1, (void *)load18},
      NEW_UNLOAD,
      NEW_FIND}},
    {NEW_IMPL,
     0,
     {{NEW, "load", LOAD_PREFIX "ZZ)Z", NEW_SYMBOL "load", 1, (void *)load19},
      {NEW, "unload", "(Ljava/lang/String;ZJ)V", NEW_SYMBOL "unload", 1,
       (void *)unload10},
      {"jdk/internal/loader/NativeLibrary", "findEntry0",
       "(JLjava/lang/String;)J",
       "Java_jdk_internal_loader_NativeLibrary_findEntry0", 1,
       (void *)find19}}}};

/* NULL for an unknown family; callers must reject the profile before any
 * VM state is touched. */
const struct gw_jvm_shape *gw_jvm_shape(uint32_t family) {
  return family >= GW_JVM_CLASSLOADER_8 && family <= GW_JVM_NATIVE_LIBRARIES_19
             ? &shapes[family - 1]
             : NULL;
}
