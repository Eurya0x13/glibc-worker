/* Isolated ownership/budget regression with a minimal VM. Including the
 * implementation permits checking retained bytes and outstanding leases; real
 * HotSpot tests cover the eight primitive types and JVM exception semantics. */
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "../../src/host/array_copy.c"

struct array {
  jsize length;
  jbyte *data;
};

static _Atomic unsigned fallback_gets, fallback_releases;
static jthrowable pending;
static int fail_region;

static jsize length(JNIEnv *env, jarray a) {
  (void)env;
  return ((struct array *)a)->length;
}

static jboolean exception(JNIEnv *env) {
  (void)env;
  return pending != NULL;
}

static jthrowable occurred(JNIEnv *env) {
  (void)env;
  return pending;
}

static void clear(JNIEnv *env) {
  (void)env;
  pending = NULL;
}

static jint throw_it(JNIEnv *env, jthrowable t) {
  (void)env;
  pending = t;
  return 0;
}

static void drop(JNIEnv *env, jobject o) {
  (void)env;
  (void)o;
}

static void get_region(JNIEnv *env, jbyteArray a, jsize start, jsize n,
                       jbyte *p) {
  (void)env;
  if (fail_region) {
    pending = (jthrowable)1;
    return;
  }
  memcpy(p, ((struct array *)a)->data + start, (size_t)n);
}

static void set_region(JNIEnv *env, jbyteArray a, jsize start, jsize n,
                       const jbyte *p) {
  (void)env;
  assert(!pending);
  memcpy(((struct array *)a)->data + start, p, (size_t)n);
}

static jbyte *get_elements(JNIEnv *env, jbyteArray a, jboolean *copy) {
  jbyte *p = malloc((size_t)length(env, (jarray)a));
  assert(p);
  get_region(env, a, 0, length(env, (jarray)a), p);
  if (copy) {
    *copy = JNI_TRUE;
  }
  atomic_fetch_add(&fallback_gets, 1);
  return p;
}

static void release_elements(JNIEnv *env, jbyteArray a, jbyte *p, jint mode) {
  if (mode != JNI_ABORT) {
    set_region(env, a, 0, length(env, (jarray)a), p);
  }
  if (mode != JNI_COMMIT) {
    free(p);
  }
  atomic_fetch_add(&fallback_releases, 1);
}
static const struct JNINativeInterface api = {
    .GetArrayLength = length,
    .ExceptionCheck = exception,
    .ExceptionOccurred = occurred,
    .ExceptionClear = clear,
    .Throw = throw_it,
    .DeleteLocalRef = drop,
    .GetByteArrayRegion = get_region,
    .SetByteArrayRegion = set_region,
    .GetByteArrayElements = get_elements,
    .ReleaseByteArrayElements = release_elements,
};
static JNIEnv vm = &api;

static struct array make_array(jsize n) {
  struct array a = {n, calloc((size_t)n, 1)};
  assert(a.data);
  return a;
}

struct release_arg {
  struct array *a;
  jbyte *p;
};

static void *release_thread(void *arg) {
  struct release_arg *r = arg;
  fh_release_byte_elements(&vm, (jbyteArray)r->a, r->p, 0);
  return NULL;
}

int main(void) {
  fh_array_copy_start(1);
  struct array a = make_array(1048576);
  jboolean copy = JNI_FALSE;
  jbyte *p = fh_get_byte_elements(&vm, (jbyteArray)&a, &copy);
  assert(p && copy && !fallback_gets && allocated == 1048576);
  p[0] = 41;
  pending = (jthrowable)2;
  fh_release_byte_elements(&vm, (jbyteArray)&a, p, JNI_COMMIT);
  assert(pending == (jthrowable)2 && a.data[0] == 41 && slots[0].active);
  pending = NULL;
  jbyte *q = fh_get_byte_elements(&vm, (jbyteArray)&a, NULL);
  assert(q && q != p);
  p[0] = 19;
  fh_release_byte_elements(&vm, (jbyteArray)&a, p, JNI_ABORT);
  assert(a.data[0] == 41);
  fh_release_byte_elements(&vm, (jbyteArray)&a, q, JNI_ABORT);
  fail_region = 1;
  assert(!fh_get_byte_elements(&vm, (jbyteArray)&a, NULL));
  fail_region = 0;
  pending = NULL;
  a.length--;
  p = fh_get_byte_elements(&vm, (jbyteArray)&a, NULL);
  assert(fallback_gets == 1);
  fh_release_byte_elements(&vm, (jbyteArray)&a, p, JNI_ABORT);
  free(a.data);
  fh_array_copy_finish();
  assert(allocated == 0);

  struct array arrays[12];
  struct release_arg args[12];
  pthread_t threads[12];
  unsigned before = fallback_gets;
  for (int i = 0; i < 12; ++i) {
    arrays[i] = make_array(1572864);
    args[i] = (struct release_arg){
        &arrays[i], fh_get_byte_elements(&vm, (jbyteArray)&arrays[i], NULL)};
    assert(args[i].p);
    for (int j = 0; j < i; ++j) {
      assert(args[i].p != args[j].p);
    }
    args[i].p[0] = 27;
  }
  assert(fallback_gets - before == 4 && allocated == 8u * 1572864);
  for (int i = 0; i < 12; ++i) {
    assert(!pthread_create(&threads[i], NULL, release_thread, &args[i]));
  }
  for (int i = 0; i < 12; ++i) {
    assert(!pthread_join(threads[i], NULL));
    assert(arrays[i].data[0] == 27);
    free(arrays[i].data);
  }
  fh_array_copy_finish();
  assert(allocated == 0);

  before = fallback_gets;
  for (int i = 0; i < 5; ++i) {
    arrays[i] = make_array(8 * 1048576);
    args[i] = (struct release_arg){
        &arrays[i], fh_get_byte_elements(&vm, (jbyteArray)&arrays[i], NULL)};
    assert(args[i].p);
  }
  assert(allocated == 32u * 1048576 && fallback_gets - before == 1);
  for (int i = 0; i < 5; ++i) {
    release_thread(&args[i]);
    free(arrays[i].data);
  }
  fh_array_copy_finish();
  assert(allocated == 0 && fallback_gets == fallback_releases);
  puts(
      "ARRAY_CACHE_LIMITS result=PASS threshold=1048576 slots=8 "
      "budget=33554432");
}
