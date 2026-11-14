/* Real-VM negative cases; compiled only into the JNI conformance DSO. */
#include <time.h>
#include "full_protocol.h"

static pthread_mutex_t failure_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t failure_condition = PTHREAD_COND_INITIALIZER;
static _Atomic int failure_entered;
static int failure_released;

static jint failure_probe(JNIEnv *e, jclass cls, jint kind, jint depth) {
  if (kind == 0 || kind == 1) {
    if (!depth) {
      return 12345;
    }
    jmethodID next = (*e)->GetStaticMethodID(e, cls, "recurse", "(II)I");
    if (!next) {
      if (kind == 1 && (*e)->ExceptionCheck(e)) {
        jthrowable pending = (*e)->ExceptionOccurred(e);
        (*e)->ExceptionClear(e);
        if (pending) {
          (*e)->DeleteLocalRef(e, pending);
        }
        return pending ? 12345 : -11;
      }
      return -12; /* Preserve the exception for Java. */
    }
    return (*e)->CallStaticIntMethod(e, cls, next, kind, depth - 1);
  }
  if (kind == 2 || kind == 3) {
    jint result = (*e)->RegisterNatives(e, cls, NULL, 65);
    if (kind == 3 && (*e)->ExceptionCheck(e)) {
      (*e)->ExceptionClear(e);
    }
    return result;
  }
  if (kind == 4) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 20;
    pthread_mutex_lock(&failure_lock);
    atomic_fetch_add(&failure_entered, 1);
    int error = 0;
    while (!failure_released && !error) {
      error = pthread_cond_timedwait(&failure_condition, &failure_lock, &until);
    }
    atomic_fetch_sub(&failure_entered, 1);
    pthread_mutex_unlock(&failure_lock);
    return error ? -13 : 0;
  }
  if (kind == 5) {
    pthread_mutex_lock(&failure_lock);
    failure_released = 1;
    pthread_cond_broadcast(&failure_condition);
    pthread_mutex_unlock(&failure_lock);
    return 0;
  }
  if (kind == 6) {
    pthread_mutex_lock(&failure_lock);
    int empty = !atomic_load(&failure_entered);
    if (empty) {
      failure_released = 0;
    }
    pthread_mutex_unlock(&failure_lock);
    return empty ? 0 : -14;
  }
  if (kind == 7) {
    return atomic_load(&failure_entered);
  }
  if (kind == 8) {
    return FB_CONTEXTS;
  }
  return 12345;
}
