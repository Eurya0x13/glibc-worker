#define _GNU_SOURCE
#include <jni.h>
#include "jre_layout_spike.h"
#include <dlfcn.h>
#include <link.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
extern int worker_host_main(int argc, char **argv);
extern int app_host_jni_begin(JNIEnv *env);
extern int app_host_jni_finish(void);
extern void app_set_callback_context(JNIEnv *env, jclass cls);

struct jre_job {
  const char *home;
  char **args;
  JavaVM *art;
  int rc;
};

static int exception(JNIEnv *env, const char *stage) {
  if (!(*env)->ExceptionCheck(env)) {
    return 0;
  }
  printf("JRE_EXCEPTION stage=%s\n", stage);
  (*env)->ExceptionDescribe(env);
  (*env)->ExceptionClear(env);
  return 1;
}

static void *jre_thread(void *opaque) {
  struct jre_job *job = opaque;
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/lib/server/libjvm.so", job->home);
  setenv("JAVA_HOME", job->home, 1);
  printf("JRE_STAGE=dlopen path=%s tid=%ld\n", path, syscall(SYS_gettid));
  void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    printf("JRE_DLOPEN_ERROR=%s\n", dlerror());
    return NULL;
  }

  jint (*create)(JavaVM **, void **, void *) =
      (jint (*)(JavaVM **, void **, void *))dlsym(handle, "JNI_CreateJavaVM");
  if (!create) {
    printf("JRE_CREATE_SYMBOL_ERROR=%s\n", dlerror());
    return NULL;
  }
  jint (*list_vms)(JavaVM **, jsize, jsize *) =
      (void *)dlsym(handle, "JNI_GetCreatedJavaVMs");
  jint (*default_args)(void *) =
      (void *)dlsym(handle, "JNI_GetDefaultJavaVMInitArgs");
  if (!list_vms || !default_args) {
    return NULL;
  }
  JavaVM *listed = NULL;
  jsize count = -1;
  JavaVMInitArgs defaults = {.version = JNI_VERSION_1_6};
  jint def_rc = default_args(&defaults), list_rc = list_vms(&listed, 1, &count);
  printf("JRE_VM_ENUM stage=before rc=%d count=%d defaults=%d\n", list_rc,
         count, def_rc);
  if (list_rc != JNI_OK || count != 0 || def_rc != JNI_OK) {
    return NULL;
  }

  Dl_info info = {0};
  dladdr((void *)create, &info);
  printf("JRE_CREATE_SYMBOL library=%s address=%p\n", info.dli_fname,
         (void *)create);
  if (gw_spike_set_jre_layout(&info, path)) {
    printf("JRE_LAYOUT_ADAPTER=FAIL\n");
    return NULL;
  }

  char home_option[PATH_MAX + 32], cp_option[PATH_MAX + 48],
      lib_option[PATH_MAX + 48], tmp_option[PATH_MAX + 48],
      search_option[PATH_MAX + 48];
  snprintf(home_option, sizeof home_option, "-Djava.home=%s", job->home);
  snprintf(cp_option, sizeof cp_option, "-Djava.class.path=%s/smoke.jar",
           job->home);
  snprintf(lib_option, sizeof lib_option, "-Dsun.boot.library.path=%s/lib",
           job->home);
  snprintf(tmp_option, sizeof tmp_option, "-Djava.io.tmpdir=%s", job->home);
  snprintf(search_option, sizeof search_option, "-Djava.library.path=%s",
           job->args[2]);
  char benchmark_mode[PATH_MAX];
  snprintf(benchmark_mode, sizeof benchmark_mode, "%s/benchmark-jit-enabled",
           job->home);
  int benchmark_jit = access(benchmark_mode, F_OK) == 0;
  JavaVMOption options[] = {
      {.optionString = home_option},
      {.optionString = cp_option},
      {.optionString = lib_option},
      {.optionString = tmp_option},
      {.optionString = search_option},
      {.optionString = "-Xms16m"},
      {.optionString = "-Xmx128m"},
      {.optionString = benchmark_jit ? "-Xmixed" : "-Xint"},
      {.optionString = "-XX:+UseSerialGC"},
      {.optionString = "-XX:-UsePerfData"},
      {.optionString = "-XX:ActiveProcessorCount=2"},
      {.optionString = "-Xcheck:jni"}};
  JavaVMInitArgs init = {
      .version = JNI_VERSION_1_6,
      .nOptions = sizeof options / sizeof options[0] - (benchmark_jit ? 1 : 0),
      .options = options,
      .ignoreUnrecognized = JNI_FALSE};

  JavaVM *vm = NULL;
  JNIEnv *env = NULL;
  printf(
      "JRE_STAGE=JNI_CreateJavaVM checkjni=%s interpreter=%s "
      "benchmark_jit=%d\n",
      benchmark_jit ? "off" : "on", benchmark_jit ? "off" : "on",
      benchmark_jit);
  jint rc = create(&vm, (void **)&env, &init);
  printf(
      "JRE_CREATE_RESULT=%d hotspot_vm=%p hotspot_env=%p art_vm=%p distinct=%d "
      "tid=%ld\n",
      rc, (void *)vm, (void *)env, (void *)job->art, vm != job->art,
      syscall(SYS_gettid));
  if (rc != JNI_OK || !env || vm == job->art) {
    return NULL;
  }

  count = -1;
  list_rc = list_vms(&listed, 1, &count);
  printf("JRE_VM_ENUM stage=created rc=%d count=%d matches=%d\n", list_rc,
         count, listed == vm);
  if (list_rc != JNI_OK || count != 1 || listed != vm) {
    return NULL;
  }

  jclass cls = (*env)->FindClass(env, "dev/glibc/worker/NativeBridge");
  if (exception(env, "FindClass") || !cls) {
    goto detach;
  }
  jmethodID verify = (*env)->GetStaticMethodID(env, cls, "verifyVm", "()I");
  if (exception(env, "verify method") || !verify) {
    goto detach;
  }
  jint identity = (*env)->CallStaticIntMethod(env, cls, verify);
  if (exception(env, "verify call") || identity != 17) {
    goto detach;
  }
  printf("JRE_BASELINE=PASS target=HotSpot\n");

  if (app_host_jni_begin(env) != JNI_OK) {
    goto detach;
  }
  app_set_callback_context(env, cls);
  int worker = worker_host_main(7, job->args);
  int proxy = app_host_jni_finish();

  jmethodID after = (*env)->GetStaticMethodID(env, cls, "afterGuest", "()I");
  if (exception(env, "after method") || !after) {
    goto detach;
  }
  jint resumed = (*env)->CallStaticIntMethod(env, cls, after);
  int java_ok = !exception(env, "after call") && resumed == 17;
  printf("JRE_BRIDGE_RESULT=%s worker=%d proxy=%d java_resumed=%d\n",
         !worker && proxy && java_ok ? "PASS" : "FAIL", worker, proxy, java_ok);
  job->rc = !worker && proxy && java_ok ? 0 : 1;

detach:
  app_set_callback_context(NULL, NULL);
  if (cls) {
    (*env)->DeleteLocalRef(env, cls);
  }
  jint destroyed = (*vm)->DestroyJavaVM(vm);
  printf("JRE_DESTROY_RESULT=%d tid=%ld\n", destroyed, syscall(SYS_gettid));
  listed = NULL;
  count = -1;
  list_rc = list_vms(&listed, 1, &count);
  printf("JRE_VM_ENUM stage=destroyed rc=%d count=%d\n", list_rc, count);
  if (destroyed != JNI_OK || list_rc != JNI_OK || count != 0) {
    job->rc = 1;
  }
  return NULL;
}

int run_android_jre(const char *home, char **args, JavaVM *art) {
  struct jre_job job = {.home = home, .args = args, .art = art, .rc = 1};
  pthread_t thread;
  int rc = pthread_create(&thread, NULL, jre_thread, &job);
  if (!rc) {
    rc = pthread_join(thread, NULL);
  }

  if (rc) {
    printf("JRE_HOST_THREAD_ERROR=%d\n", rc);
  }

  return rc ? 1 : job.rc;
}
