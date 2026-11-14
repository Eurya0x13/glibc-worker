#define _GNU_SOURCE
#include <jni.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
static JavaVM *vm;
static __thread int tls_value = 17;
static _Atomic int *destructor_counter;
static int constructed;

__attribute__((constructor)) static void init(void) {
  constructed = 1;
}

__attribute__((destructor)) static void fini(void) {
  if (destructor_counter) {
    atomic_fetch_add(destructor_counter, 1);
  }
}

JNIEXPORT void conformance_observe_destructor(_Atomic int *p) {
  destructor_counter = p;
}

#define C(x)                                                               \
  do {                                                                     \
    if (!(x)) {                                                            \
      fprintf(stderr, "GUEST_CASE_FAIL line=%d expression=%s\n", __LINE__, \
              #x);                                                         \
      return __LINE__;                                                     \
    }                                                                      \
  } while (0)
#define E()                        \
  do {                             \
    if ((*e)->ExceptionCheck(e)) { \
      (*e)->ExceptionDescribe(e);  \
      (*e)->ExceptionClear(e);     \
      return __LINE__;             \
    }                              \
  } while (0)
#include "fixture_generated.inc"

static jobject new_v(JNIEnv *e, jclass cls, jmethodID ctor, ...) {
  va_list va;
  va_start(va, ctor);
  jobject o = (*e)->NewObjectV(e, cls, ctor, va);
  va_end(va);
  return o;
}

static int classes(JNIEnv *e, jclass module, jbyteArray bytes) {
  jclass box = (*e)->FindClass(e, "conformance/Box"),
         child = (*e)->FindClass(e, "conformance/Child");
  C(box && child);
  jclass parent = (*e)->GetSuperclass(e, child);
  C((*e)->IsSameObject(e, parent, box));
  C((*e)->IsAssignableFrom(e, child, box));

  jmethodID ctor =
      (*e)->GetMethodID(e, box, "<init>", "(ZBCSIJFDLjava/lang/Object;)V");
  C(ctor);
  jstring str = (*e)->NewStringUTF(e, "ctor");
  C(str);
  jvalue a[9] = {{.z = 1},     {.b = -7},    {.c = 0x4e2d},
                 {.s = -2000}, {.i = 12345}, {.j = INT64_C(0x123456789abc)},
                 {.f = 1.25f}, {.d = 2.5},   {.l = str}};
  jobject one = (*e)->NewObject(e, box, ctor, 1, -7, 0x4e2d, -2000, 12345,
                                INT64_C(0x123456789abc), 1.25, 2.5, str);
  E();
  jobject two = (*e)->NewObjectA(e, box, ctor, a);
  E();
  jobject three = new_v(e, box, ctor, 1, -7, 0x4e2d, -2000, 12345,
                        INT64_C(0x123456789abc), 1.25, 2.5, str);
  E();

  C(one && two && three);
  C((*e)->IsInstanceOf(e, one, box));
  C((*e)->IsSameObject(e, (*e)->GetObjectClass(e, one), box));
  jfieldID id = (*e)->GetFieldID(e, box, "i", "I");
  C(id);
  C((*e)->GetIntField(e, one, id) == 12345 &&
    (*e)->GetIntField(e, two, id) == 12345 &&
    (*e)->GetIntField(e, three, id) == 12345);

  jobject rf = (*e)->ToReflectedField(e, box, id, JNI_FALSE);
  C(rf);
  C((*e)->FromReflectedField(e, rf) == id);
  jmethodID method =
      (*e)->GetMethodID(e, box, "mInt", "(ZBCSIJFDLjava/lang/Object;)I");
  C(method);
  jobject rm = (*e)->ToReflectedMethod(e, box, method, JNI_FALSE);
  C(rm);
  C((*e)->FromReflectedMethod(e, rm) == method);

  jclass class_cls = (*e)->FindClass(e, "java/lang/Class");
  C(class_cls);
  jmethodID loader_id = (*e)->GetMethodID(e, class_cls, "getClassLoader",
                                          "()Ljava/lang/ClassLoader;");
  C(loader_id);
  jobject loader = (*e)->CallObjectMethod(e, module, loader_id);
  E();
  C(loader);

  jsize n = (*e)->GetArrayLength(e, bytes);
  jbyte *p = (*e)->GetByteArrayElements(e, bytes, NULL);
  C(p);
  jclass dynamic = (*e)->DefineClass(e, "conformance/Dynamic", loader, p, n);
  (*e)->ReleaseByteArrayElements(e, bytes, p, JNI_ABORT);
  E();
  C(dynamic);
  jmethodID dc = (*e)->GetMethodID(e, dynamic, "<init>", "()V");
  C(dc);
  jobject obj = (*e)->NewObject(e, dynamic, dc);
  E();
  C(obj);
  jmethodID val = (*e)->GetMethodID(e, dynamic, "value", "()I");
  C(val);
  jint v = (*e)->CallIntMethod(e, obj, val);
  E();
  C(v == 71);

  jobject (*getmodule)(JNIEnv *, jclass) =
      (jobject (*)(JNIEnv *, jclass))((void **)*e)[233];
  jobject mod = getmodule(e, module);
  C(mod);
  return 0;
}

static void gc(JNIEnv *e, jclass cls) {
  jmethodID m = (*e)->GetStaticMethodID(e, cls, "gc", "()V");
  if (m) {
    (*e)->CallStaticVoidMethod(e, cls, m);
  }
}

static int references(JNIEnv *e, jclass cls) {
  C((*e)->EnsureLocalCapacity(e, 32) == JNI_OK);
  C((*e)->PushLocalFrame(e, 16) == JNI_OK);

  jstring a = (*e)->NewStringUTF(e, "abc");
  C(a);
  jobject b = (*e)->NewLocalRef(e, a);
  C(b);
  C((*e)->IsSameObject(e, a, b));
  C((*e)->GetObjectRefType(e, a) == JNILocalRefType);

  jobject g = (*e)->NewGlobalRef(e, a);
  C(g);
  C((*e)->GetObjectRefType(e, g) == JNIGlobalRefType);

  jweak w = (*e)->NewWeakGlobalRef(e, a);
  C(w);
  C((*e)->GetObjectRefType(e, w) == JNIWeakGlobalRefType);

  jobject kept = (*e)->PopLocalFrame(e, b);
  C(kept);
  C((*e)->GetStringLength(e, (jstring)kept) == 3);
  (*e)->DeleteLocalRef(e, kept);
  gc(e, cls);
  E();
  C(!(*e)->IsSameObject(e, g, NULL));
  jobject copy = (*e)->NewLocalRef(e, g);
  C(copy);
  (*e)->DeleteLocalRef(e, copy);
  (*e)->DeleteGlobalRef(e, g);
  gc(e, cls);
  E();
  C((*e)->IsSameObject(e, w, NULL));
  C((*e)->NewLocalRef(e, w) == NULL);
  (*e)->DeleteWeakGlobalRef(e, w);
  C((*e)->GetObjectRefType(e, NULL) == JNIInvalidRefType);

  for (int i = 0; i < 2000; i++) {
    jstring s = (*e)->NewStringUTF(e, "local");
    C(s);
    (*e)->DeleteLocalRef(e, s);
  }

  /* Unlink/reuse head, middle and tail slots, then pop nested frames while
     retaining an inner result. Outer refs must survive the inner cleanup. */
  for (int cycle = 0; cycle < 8; cycle++) {
    C((*e)->PushLocalFrame(e, 128) == JNI_OK);
    jobject outer[64];
    for (int i = 0; i < 64; i++) {
      outer[i] = (*e)->NewLocalRef(e, cls);
      C(outer[i]);
    }
    for (int i = 0; i < 64; i += 3) {
      (*e)->DeleteLocalRef(e, outer[i]);
      outer[i] = (*e)->NewLocalRef(e, cls);
      C(outer[i]);
    }
    C((*e)->PushLocalFrame(e, 64) == JNI_OK);
    jobject inner[32];
    for (int i = 0; i < 32; i++) {
      inner[i] = (*e)->NewLocalRef(e, outer[i]);
      C(inner[i]);
    }
    (*e)->DeleteLocalRef(e, inner[0]);
    (*e)->DeleteLocalRef(e, inner[17]);
    (*e)->DeleteLocalRef(e, inner[31]);
    jobject survivor = (*e)->PopLocalFrame(e, inner[16]);
    C(survivor && (*e)->IsSameObject(e, survivor, outer[7]));
    survivor = (*e)->PopLocalFrame(e, survivor);
    C(survivor && (*e)->IsSameObject(e, survivor, cls));
    (*e)->DeleteLocalRef(e, survivor);
    E();
  }
  return 0;
}

static int strings(JNIEnv *e) {
  jchar chars[] = {0x41, 0, 0x4e2d, 0xd83d, 0xde00};
  jstring s = (*e)->NewString(e, chars, 5);
  C(s);
  C((*e)->GetStringLength(e, s) == 5);
  jchar out[5] = {0};
  (*e)->GetStringRegion(e, s, 0, 5, out);
  E();
  C(!memcmp(chars, out, sizeof chars));

  const jchar *p = (*e)->GetStringChars(e, s, NULL);
  C(p && !memcmp(p, chars, sizeof chars));
  (*e)->ReleaseStringChars(e, s, p);

  const char expected[] = {'A',        (char)0xc0, (char)0x80, (char)0xe4,
                           (char)0xb8, (char)0xad, (char)0xed, (char)0xa0,
                           (char)0xbd, (char)0xed, (char)0xb8, (char)0x80,
                           0};
  C((*e)->GetStringUTFLength(e, s) == 12);
  const char *u = (*e)->GetStringUTFChars(e, s, NULL);
  C(u && !memcmp(u, expected, 13));
  (*e)->ReleaseStringUTFChars(e, s, u);

  char bytes[32];
  memset(bytes, 0x5a, sizeof bytes);
  (*e)->GetStringUTFRegion(e, s, 0, 5, bytes);
  E();
  C(!memcmp(bytes, expected, 12));

  jstring round = (*e)->NewStringUTF(e, expected);
  C(round);
  C((*e)->GetStringLength(e, round) == 5);

  const jchar *critical = (*e)->GetStringCritical(e, s, NULL);
  C(critical);
  int good = !memcmp(critical, chars, sizeof chars);
  (*e)->ReleaseStringCritical(e, s, critical);
  C(good);
  return 0;
}

static int exceptions(JNIEnv *e) {
  jclass cls = (*e)->FindClass(e, "java/lang/IllegalStateException");
  C(cls);
  C((*e)->ThrowNew(e, cls, "fixture expected") == 0);
  C((*e)->ExceptionCheck(e));

  jthrowable ex = (*e)->ExceptionOccurred(e);
  C(ex);
  (*e)->ExceptionClear(e);
  C(!(*e)->ExceptionCheck(e));

  C((*e)->Throw(e, ex) == 0);
  C((*e)->ExceptionCheck(e));
  (*e)->ExceptionDescribe(e);
  C(!(*e)->ExceptionCheck(e));

  (*e)->DeleteLocalRef(e, ex);
  jclass missing = (*e)->FindClass(e, "conformance/DoesNotExist");
  C(!missing && (*e)->ExceptionCheck(e));
  (*e)->ExceptionClear(e);
  return 0;
}

static int buffers(JNIEnv *e) {
  jintArray a = (*e)->NewIntArray(e, 3);
  C(a);
  jboolean copied = JNI_FALSE;
  jint *p = (*e)->GetPrimitiveArrayCritical(e, a, &copied);
  C(p);
  p[0] = 91;
  p[1] = 92;
  (*e)->ReleasePrimitiveArrayCritical(e, a, p, 0);
  p = (*e)->GetPrimitiveArrayCritical(e, a, &copied);
  C(p);
  (*e)->ReleasePrimitiveArrayCritical(e, a, p, JNI_ABORT);
  jint out[3];
  (*e)->GetIntArrayRegion(e, a, 0, 3, out);
  C(out[0] == 91 && out[1] == 92);

  unsigned char *data = malloc(32);
  C(data);
  memset(data, 0xa5, 32);
  jobject buffer = (*e)->NewDirectByteBuffer(e, data, 32);
  C(buffer);
  C((*e)->GetDirectBufferAddress(e, buffer) == data &&
    (*e)->GetDirectBufferCapacity(e, buffer) == 32);
  (*e)->DeleteLocalRef(e, buffer);
  free(data);

  jclass object = (*e)->FindClass(e, "java/lang/Object");
  C(object);
  jstring s = (*e)->NewStringUTF(e, "object");
  C(s);
  jobjectArray arr = (*e)->NewObjectArray(e, 3, object, s);
  C(arr);
  C((*e)->GetArrayLength(e, arr) == 3);
  jobject v = (*e)->GetObjectArrayElement(e, arr, 1);
  C((*e)->IsSameObject(e, v, s));
  (*e)->SetObjectArrayElement(e, arr, 1, NULL);
  C((*e)->GetObjectArrayElement(e, arr, 1) == NULL);
  return 0;
}

static void *attach_worker(void *arg) {
  int daemon = (int)(intptr_t)arg;
  JNIEnv *e = NULL;
  void *unbound = (void *)1;
  int good =
      (*vm)->GetEnv(vm, &unbound, JNI_VERSION_1_6) == JNI_EDETACHED && !unbound;
  JavaVMAttachArgs args = {
      .version = JNI_VERSION_1_6, .name = "guest-attached", .group = NULL};
  jint rc = daemon ? (*vm)->AttachCurrentThreadAsDaemon(vm, &e, &args)
                   : (*vm)->AttachCurrentThread(vm, &e, &args);
  if (rc != JNI_OK || !e) {
    return NULL;
  }

  JNIEnv *again = NULL;
  good &= (*vm)->AttachCurrentThread(vm, &again, NULL) == JNI_OK && again == e;
  void *current = NULL;
  good &=
      (*vm)->GetEnv(vm, &current, JNI_VERSION_1_6) == JNI_OK && current == e;

  jclass thread = (*e)->FindClass(e, "java/lang/Thread");
  jmethodID cur = (*e)->GetStaticMethodID(e, thread, "currentThread",
                                          "()Ljava/lang/Thread;");
  jobject obj = (*e)->CallStaticObjectMethod(e, thread, cur);
  if ((*e)->ExceptionCheck(e)) {
    good = 0;
  }
  jmethodID isdaemon = (*e)->GetMethodID(e, thread, "isDaemon", "()Z");
  jboolean d = (*e)->CallBooleanMethod(e, obj, isdaemon);
  if ((*e)->ExceptionCheck(e)) {
    good = 0;
  }
  good &= (d == daemon);

  (*e)->DeleteLocalRef(e, obj);
  (*e)->DeleteLocalRef(e, thread);
  good &= (*vm)->DetachCurrentThread(vm) == JNI_OK;
  current = (void *)1;
  good &=
      (*vm)->GetEnv(vm, &current, JNI_VERSION_1_6) == JNI_EDETACHED && !current;
  return (void *)(intptr_t)good;
}

static int threads(JNIEnv *e) {
  void *out = (void *)1;
  C((*vm)->GetEnv(vm, &out, 0x7fffffff) == JNI_EVERSION && !out);
  JavaVM *round = NULL;
  C((*e)->GetJavaVM(e, &round) == JNI_OK && round == vm);
  C((*vm)->DetachCurrentThread(vm) == JNI_ERR);
  C((*vm)->DestroyJavaVM(vm) == JNI_ERR);
  dprintf(2, "JNI_POLICY Guest_DestroyJavaVM=UNSUPPORTED_OWNER_ONLY\n");

  pthread_t a, b;
  C(!pthread_create(&a, NULL, attach_worker, (void *)0));
  C(!pthread_create(&b, NULL, attach_worker, (void *)1));
  void *x = NULL, *y = NULL;
  C(!pthread_join(a, &x));
  C(!pthread_join(b, &y));
  C(x == (void *)1 && y == (void *)1);
  return 0;
}

static int versions(JNIEnv *env) {
  jint capability = (*env)->GetVersion(env);
  JNIEnv *round = NULL;

  if ((*vm)->GetEnv(vm, (void **)&round, capability) != JNI_OK ||
      round != env) {
    return 0;
  }
  round = (void *)1;
  if ((*vm)->GetEnv(vm, (void **)&round, 0x00130000) != JNI_EVERSION || round) {
    return 0;
  }

  JavaVMAttachArgs future = {.version = 0x00130000};
  round = (void *)1;
  if ((*vm)->AttachCurrentThread(vm, &round, &future) != JNI_EVERSION ||
      round) {
    return 0;
  }

  dprintf(2,
          "JNI_VERSION_TEST "
          "{\"capability\":%d,\"getenv_current\":true,\"getenv_future_"
          "rejected\":true,\"attach_future_rejected\":true}\n",
          capability);
  return 1;
}

static jint JNICALL tls_set(JNIEnv *e, jclass c, jint value) {
  (void)e;
  (void)c;
  int old = tls_value;
  tls_value = value;
  return old;
}

static jint JNICALL tls_get(JNIEnv *e, jclass c) {
  (void)e;
  (void)c;
  return tls_value;
}

static jint JNICALL bounce(JNIEnv *e, jclass c, jint value) {
  if (value <= 0) {
    return 0;
  }

  jmethodID m = (*e)->GetStaticMethodID(e, c, "javaBounce", "(I)I");
  if (!m) {
    return -100;
  }

  int r = (*e)->CallStaticIntMethod(e, c, m, value - 1);
  if ((*e)->ExceptionCheck(e)) {
    return -200;
  }
  return r + 1;
}

static jint JNICALL increment(JNIEnv *e, jclass c, jobject lock,
                              jintArray count) {
  (void)c;
  if ((*e)->MonitorEnter(e, lock)) {
    return -1;
  }

  jint v = 0;
  (*e)->GetIntArrayRegion(e, count, 0, 1, &v);
  sched_yield();
  v++;
  (*e)->SetIntArrayRegion(e, count, 0, 1, &v);

  (*e)->MonitorExit(e, lock);
  return v;
}

static jlong JNICALL mixed(JNIEnv *e, jclass c, jlong value, jdouble scale,
                           jobject text) {
  (void)c;
  return value + (jlong)(scale * 100) + (*e)->GetStringLength(e, (jstring)text);
}

static void JNICALL throw_native(JNIEnv *e, jclass c) {
  (void)c;
  jclass ex = (*e)->FindClass(e, "java/lang/IllegalStateException");
  if (ex) {
    (*e)->ThrowNew(e, ex, "guest native throw");
  }
}

JNIEXPORT jint JNICALL Java_conformance_Module_namedOnly(JNIEnv *e, jclass c,
                                                         jint v) {
  (void)e;
  (void)c;
  return v + 1;
}

JNIEXPORT jint JNICALL Java_conformance_Module_overloaded__I(JNIEnv *e,
                                                             jclass c, jint v) {
  (void)e;
  (void)c;
  return v + 2;
}

JNIEXPORT jlong JNICALL Java_conformance_Module_overloaded__J(JNIEnv *e,
                                                              jclass c,
                                                              jlong v) {
  (void)e;
  (void)c;
  return v + 3;
}

#define ECHO(T, N)                                                         \
  JNIEXPORT T JNICALL Java_conformance_Module_echo##N(JNIEnv *e, jclass c, \
                                                      T v) {               \
    (void)e;                                                               \
    (void)c;                                                               \
    return v;                                                              \
  }
ECHO(jboolean, Z)
ECHO(jbyte, B)
ECHO(jchar, C)
ECHO(jshort, S)
ECHO(jint, I)
ECHO(jlong, J)
ECHO(jfloat, F)
ECHO(jdouble, D)
ECHO(jobject, L)

JNIEXPORT jobject JNICALL Java_conformance_Module_instanceEcho(JNIEnv *e,
                                                               jobject self,
                                                               jobject v) {
  return (*e)->IsSameObject(e, self, NULL) ? NULL : v;
}

static jint JNICALL group(JNIEnv *, jclass, jint, jbyteArray);
static JNINativeMethod methods[] = {
    {"group", "(I[B)I", (void *)group},
    {"bounceNative", "(I)I", (void *)bounce},
    {"tlsSet", "(I)I", (void *)tls_set},
    {"tlsGet", "()I", (void *)tls_get},
    {"lockIncrement", "(Ljava/lang/Object;[I)I", (void *)increment},
    {"mixed", "(JDLjava/lang/Object;)J", (void *)mixed},
    {"throwNative", "()V", (void *)throw_native}};

static jint JNICALL group(JNIEnv *e, jclass cls, jint which, jbyteArray bytes) {
  switch (which) {
    case 0:
      return classes(e, cls, bytes);
    case 1:
      return method_matrix(e);
    case 2:
      return fields_arrays(e);
    case 3:
      return references(e, cls);
    case 4:
      return strings(e);
    case 5:
      return exceptions(e);
    case 6:
      return buffers(e);
    case 7:
      return threads(e);

    case 8:
      C((*e)->UnregisterNatives(e, cls) == JNI_OK);
      C((*e)->RegisterNatives(e, cls, methods,
                              sizeof methods / sizeof methods[0]) == JNI_OK);
      return 0;

    case 9: {
      jobject lock = (*e)->NewStringUTF(e, "lock");
      C(lock);
      C((*e)->MonitorEnter(e, lock) == 0);
      C((*e)->MonitorEnter(e, lock) == 0);
      C((*e)->MonitorExit(e, lock) == 0);
      C((*e)->MonitorExit(e, lock) == 0);
      return 0;
    }

    case 99:
      dprintf(2, "JNI_EXPECTED_FATAL stage=guest-before-call\n");
      (*e)->FatalError(e, "fixture requested fatal");
      return -99;
    default:
      return -1;
  }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *v, void *reserved) {
  (void)reserved;
  vm = v;
  JNIEnv *e = NULL;
  if (!constructed ||
      (*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  if ((*e)->GetVersion(e) < JNI_VERSION_1_6) {
    return JNI_ERR;
  }
  if (!versions(e)) {
    return JNI_ERR;
  }

  jclass cls = (*e)->FindClass(e, "conformance/Module");
  if (!cls) {
    return JNI_ERR;
  }
  if ((*e)->RegisterNatives(e, cls, methods,
                            sizeof methods / sizeof methods[0])) {
    return JNI_ERR;
  }

  (*e)->DeleteLocalRef(e, cls);
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *v, void *reserved) {
  (void)reserved;
  JNIEnv *e = NULL;
  if ((*v)->GetEnv(v, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
    fprintf(stderr, "GUEST_UNLOAD_ENV_ERROR\n");
  }
}
