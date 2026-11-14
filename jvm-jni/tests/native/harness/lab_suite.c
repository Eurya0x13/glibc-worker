#include "lab_suite.h"
#include "protocol.h"
#include "loader_backend_internal.h"
#include "loader_smoke.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
static const char *api_names[FB_APIS] = {
#include "generated/api-names.inc"
};

int gw_lab_suite(struct full_bridge *B, JNIEnv *env) {
  jclass cls = (*env)->FindClass(env, "conformance/Runner");
  if (!cls || (*env)->ExceptionCheck(env)) {
    return 1;
  }
  jmethodID run =
      (*env)->GetStaticMethodID(env, cls, "run", "(Ljava/lang/String;)I");
  char hostpath[4096];
  snprintf(hostpath, sizeof hostpath, "%s", B->module_path);
  jstring path = (*env)->NewStringUTF(env, hostpath);
  jint rc = (*env)->CallStaticIntMethod(env, cls, run, path);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    rc = -1;
  }
  jfieldID expected_field =
      (*env)->GetStaticFieldID(env, cls, "expectedBridgeErrors", "I");
  int expected_errors =
      expected_field ? (*env)->GetStaticIntField(env, cls, expected_field) : -1;
  (*env)->DeleteLocalRef(env, path);
  (*env)->DeleteLocalRef(env, cls);

  if (!rc) {
    char *slash = strrchr(hostpath, '/');
    if (slash) {
      *slash = 0;
    }
    rc = gw_loader_smoke(env, hostpath);
  }

  int touched = 0;
  for (int i = 0; i < FB_APIS; i++) {
    unsigned n = atomic_load(&B->hits[i]);
    if (n) {
      touched++;
    }
    printf("JNI_COVERAGE index=%d name=%s calls=%u\n", i, api_names[i], n);
  }

  printf(
      "FULL_LIFECYCLE loads=%d unloads=%d constructors=%d destructors=%d "
      "calls=%d depth=%d "
      "globals=%d weak=%d locals=%d closures=%d threads=%d errors=%d "
      "expected_errors=%d "
      "coverage=%d/%d\n",
      atomic_load(&B->loads), atomic_load(&B->unloads),
      atomic_load(&B->constructors), atomic_load(&B->destructors),
      atomic_load(&B->native_calls), atomic_load(&B->max_depth),
      atomic_load(&B->live_global), atomic_load(&B->live_weak),
      atomic_load(&B->live_local), atomic_load(&B->live_closures),
      atomic_load(&B->live_guest_threads), atomic_load(&B->errors),
      expected_errors, touched, FB_APIS);
  printf(
      "MULTI_LIFECYCLE active=%d peak=%d failed=%d dependency_constructors=%d "
      "dependency_destructors=%d id_cache_weak=%d\n",
      atomic_load(&B->active_modules), atomic_load(&B->peak_modules),
      atomic_load(&B->failed_loads), atomic_load(&B->dependency_constructors),
      atomic_load(&B->dependency_destructors), atomic_load(&B->id_cache_weak));
  printf("GUEST_IMAGE_LIFECYCLE opens=%d closes=%d\n",
         atomic_load(&B->image_opens), atomic_load(&B->image_closes));

  B->suite_rc = rc || expected_errors < 0 ||
                atomic_load(&B->errors) != expected_errors ||
                atomic_load(&B->active_modules);
  printf("FULL_JNI_SUITE_RESULT=%s java_rc=%d\n", B->suite_rc ? "FAIL" : "PASS",
         rc);
  return B->suite_rc;
}

static struct full_bridge *lab_bridge;

static jlong process_cpu_nanos(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  struct timespec time;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time)) {
    return -1;
  }
  return (jlong)time.tv_sec * 1000000000 + time.tv_nsec;
}

static jint bridge_metric(JNIEnv *env, jclass cls, jint which) {
  (void)env;
  (void)cls;
  if (!lab_bridge) {
    return -1;
  }

  switch (which) {
    case 1:
      return atomic_load(&lab_bridge->loads);
    case 2:
      return atomic_load(&lab_bridge->unloads);
    case 3:
      return atomic_load(&lab_bridge->destructors);
    case 4:
      return atomic_load(&lab_bridge->errors);
    case 5:
      return atomic_load(&lab_bridge->live_closures);
    case 6:
      return atomic_load(&lab_bridge->max_depth);
    case 7:
      return atomic_load(&lab_bridge->live_global);
    case 8:
      return atomic_load(&lab_bridge->live_weak);
    case 9:
      return atomic_load(&lab_bridge->active_modules);
    case 10:
      return atomic_load(&lab_bridge->peak_modules);
    case 11:
      return atomic_load(&lab_bridge->id_cache_weak);
    case 12:
      return atomic_load(&lab_bridge->failed_loads);
    case 13:
      return atomic_load(&lab_bridge->dependency_constructors);
    case 14:
      return atomic_load(&lab_bridge->dependency_destructors);
    case 15:
      return atomic_load(&lab_bridge->image_opens);
    case 16:
      return atomic_load(&lab_bridge->image_closes);

    case 17:
      /* Test diagnostics: the initial GNU owner, not a Java thread ID. */
      return lab_bridge->ctx[0].guest_tid;

    case 18: {
      int free_slots = 0;
      for (int i = 1; i < FB_CONTEXTS; i++) {
        free_slots += atomic_load(&lab_bridge->ctx[i].state) == FB_FREE;
      }
      return free_slots;
    }
    default:
      return -1;
  }
}

static jlong bridge_last(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  return (jlong)gw_loader_last_handle();
}

static jint bridge_state(JNIEnv *env, jclass cls, jlong id) {
  (void)env;
  (void)cls;
  return gw_loader_query(GW_LOADER_ABI_V1, sizeof(struct gw_loader_api_v1))
      ->module_state((uint64_t)id);
}

/* Negative-test hook: attempts actual production close from a Java callback.
   A scope refusal must leave the module and VM registrations untouched. */
static jint bridge_try_close(JNIEnv *env, jclass cls, jlong id) {
  (void)cls;
  struct gw_loader_error error = {0};
  int result =
      gw_loader_query(GW_LOADER_ABI_V1, sizeof(struct gw_loader_api_v1))
          ->close(env, (uint64_t)id, &error);
  return result == GW_ERROR ? error.code : -100 - result;
}

static void payload_phase(JNIEnv *env, jclass cls, jint stage, jint operation,
                          jint bytes) {
  (void)env;
  (void)cls;
  struct timespec wall, cpu;
  struct rusage usage;

  clock_gettime(CLOCK_MONOTONIC, &wall);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
  getrusage(RUSAGE_THREAD, &usage);

  printf(
      "PAYLOAD_PHASE stage=%d operation=%d bytes=%d mono_ns=%llu cpu_ns=%llu "
      "minflt=%ld majflt=%ld tid=%ld\n",
      stage, operation, bytes,
      (unsigned long long)wall.tv_sec * 1000000000 + wall.tv_nsec,
      (unsigned long long)cpu.tv_sec * 1000000000 + cpu.tv_nsec,
      usage.ru_minflt, usage.ru_majflt, syscall(SYS_gettid));
  fflush(stdout);
}

int gw_lab_prepare(struct full_bridge *bridge, JNIEnv *env,
                   const char *directory) {
  lab_bridge = bridge;
  jclass cls = (*env)->FindClass(env, "dev/glibc/worker/NativeBridge");
  if (!cls) {
    return 1;
  }

  JNINativeMethod metrics[] = {
      {"bridgeMetric", "(I)I", (void *)bridge_metric},
      {"bridgeLastHandle", "()J", (void *)bridge_last},
      {"bridgeHandleState", "(J)I", (void *)bridge_state},
      {"bridgeTryClose", "(J)I", (void *)bridge_try_close},
      {"processCpuNanos", "()J", (void *)process_cpu_nanos},
      {"payloadPhase", "(III)V", (void *)payload_phase}};

  int rc = (*env)->RegisterNatives(env, cls, metrics,
                                   sizeof metrics / sizeof *metrics);
  (*env)->DeleteLocalRef(env, cls);
  return rc != JNI_OK || gw_loader_smoke_not_ready(env, directory);
}

extern JNIEnv *app_host_jni_env(void);

int full_host_suite(struct shared *s) {
  JNIEnv *env = app_host_jni_env();
  struct full_bridge *bridge = (struct full_bridge *)s->full_bridge;
  int rc = gw_lab_suite(bridge, env);
  if (rc) {
    return rc;
  }

  jclass cls = (*env)->FindClass(env, "benchmark/Workload");
  jmethodID run =
      cls ? (*env)->GetStaticMethodID(env, cls, "run", "(Ljava/lang/String;)I")
          : NULL;

  char directory[4096];
  snprintf(directory, sizeof directory, "%s", bridge->module_path);
  char *slash = strrchr(directory, '/');
  if (slash) {
    *slash = 0;
  }
  jstring path = run ? (*env)->NewStringUTF(env, directory) : NULL;
  rc = path ? (*env)->CallStaticIntMethod(env, cls, run, path) : -1;
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    rc = -1;
  }

  if (path) {
    (*env)->DeleteLocalRef(env, path);
  }
  if (cls) {
    (*env)->DeleteLocalRef(env, cls);
  }
  return rc;
}
