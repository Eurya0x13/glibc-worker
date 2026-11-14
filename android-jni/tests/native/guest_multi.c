/* Unmodified JNI-style C ABI. The optional observer is test instrumentation. */
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
extern void multi_dep_observe(_Atomic int *, _Atomic int *);
extern int multi_dep_add(int), multi_dep_value(void), multi_dep_tls(int);
extern uintptr_t multi_dep_cookie(void);
static JavaVM *vm;
static jmethodID echo, route_java;
static jobject retained;
static __thread int tls = 17;
static _Atomic int *destructors;

JNIEXPORT void conformance_observe_destructor(_Atomic int *p) {
  destructors = p;
}

JNIEXPORT void conformance_observe_dependency(_Atomic int *c, _Atomic int *d) {
  multi_dep_observe(c, d);
}

__attribute__((destructor)) static void fini(void) {
  if (destructors) {
    atomic_fetch_add(destructors, 1);
  }
}

static jint JNICALL identity(JNIEnv *e, jclass c) {
  (void)e;
  (void)c;
  return MODULE_ID;
}

static jint JNICALL cached(JNIEnv *e, jclass c, jint v) {
  return (*e)->CallStaticIntMethod(e, c, echo, v);
}

static jint JNICALL add(JNIEnv *e, jclass c, jint v) {
  (void)e;
  (void)c;
  return multi_dep_add(v);
}

static jint JNICALL value(JNIEnv *e, jclass c) {
  (void)e;
  (void)c;
  return multi_dep_value();
}

static jlong JNICALL cookie(JNIEnv *e, jclass c) {
  (void)e;
  (void)c;
  return (jlong)multi_dep_cookie();
}

static jint JNICALL own_tls(JNIEnv *e, jclass c, jint v) {
  (void)e;
  (void)c;
  int old = tls;
  if (v >= 0) {
    tls = v;
  }
  return old;
}

static jint JNICALL dep_tls(JNIEnv *e, jclass c, jint v) {
  (void)e;
  (void)c;
  return multi_dep_tls(v);
}

static jint JNICALL route(JNIEnv *e, jclass c, jint depth) {
  if (depth <= 0) {
    return MODULE_ID;
  }
  return MODULE_ID + (*e)->CallStaticIntMethod(e, c, route_java, depth - 1);
}

JNIEXPORT jint JNICALL Java_multi_Module_named(JNIEnv *e, jclass c) {
  (void)e;
  (void)c;
  return MODULE_ID + 1;
}

static JNINativeMethod methods[] = {
    {"identity", "()I", (void *)identity}, {"cached", "(I)I", (void *)cached},
    {"add", "(I)I", (void *)add},          {"value", "()I", (void *)value},
    {"cookie", "()J", (void *)cookie},     {"ownTls", "(I)I", (void *)own_tls},
    {"depTls", "(I)I", (void *)dep_tls},   {"route", "(I)I", (void *)route}};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *v, void *opaque) {
  (void)opaque;
  vm = v;
  JNIEnv *e = NULL;
  if ((*v)->GetEnv(v, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  jclass cls = (*e)->FindClass(e, "multi/Module");
  if (!cls) {
    return JNI_ERR;
  }
  echo = (*e)->GetStaticMethodID(e, cls, "javaEcho", "(I)I");
  if (!echo) {
    return JNI_ERR;
  }
  route_java = (*e)->GetStaticMethodID(e, cls, "javaRoute", "(I)I");
  if (!route_java) {
    return JNI_ERR;
  }

  jstring text = (*e)->NewStringUTF(e, "multi-global");
  if (!text) {
    return JNI_ERR;
  }
  retained = (*e)->NewGlobalRef(e, text);
  (*e)->DeleteLocalRef(e, text);
  if (!retained) {
    return JNI_ERR;
  }

  if ((*e)->RegisterNatives(e, cls, methods,
                            sizeof methods / sizeof methods[0]) != JNI_OK) {
    (*e)->DeleteGlobalRef(e, retained);
    retained = NULL;
    return JNI_ERR;
  }

  (*e)->DeleteLocalRef(e, cls);
#ifdef REJECT_AFTER_REGISTER
  (*e)->DeleteGlobalRef(e, retained);
  retained = NULL;
  return JNI_ERR;
#else
  return JNI_VERSION_1_6;
#endif
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *v, void *opaque) {
  (void)opaque;
  JNIEnv *e = NULL;
  if ((*v)->GetEnv(v, (void **)&e, JNI_VERSION_1_6) == JNI_OK && retained) {
    (*e)->DeleteGlobalRef(e, retained);
    retained = NULL;
  }
}
