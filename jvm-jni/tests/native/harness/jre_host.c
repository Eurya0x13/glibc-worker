#define _GNU_SOURCE
#include <jni.h>
#include "jre_layout_spike.h"
#include "pojav_jli.h"
#include "jre-layout.h"
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
  void *libjli;
  jint (*list_vms)(JavaVM **, jsize, jsize *);
};

#if JRE_USE_JLI
static struct jre_job *launched_job;
#endif

static int exception(JNIEnv *env, const char *stage) {
  if (!(*env)->ExceptionCheck(env)) {
    return 0;
  }

  printf("JRE_EXCEPTION stage=%s\n", stage);
  (*env)->ExceptionDescribe(env);
  (*env)->ExceptionClear(env);

  return 1;
}

static int execute_bridge(struct jre_job *job, JavaVM *vm, JNIEnv *env) {
  JavaVM *listed = NULL;
  jsize count = -1;
  jint list_rc = job->list_vms(&listed, 1, &count);
  printf("JRE_VM_ENUM stage=created rc=%d count=%d matches=%d\n", list_rc,
         count, listed == vm);
  if (list_rc != JNI_OK || count != 1 || listed != vm) {
    return 1;
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
  if (exception(env, "verify call") || identity != JRE_EXPECTED_MAJOR) {
    goto detach;
  }
  printf(
      "JRE_ARTIFACT {\"id\":\"%s\",\"abi\":\"%s\",\"major\":%d,"
      "\"archive_sha256\":\"%s\",\"libjava_sha256\":\"%s\","
      "\"libjvm_sha256\":\"%s\",\"family\":%d}\n",
      JRE_ARTIFACT_ID, JRE_ARTIFACT_ABI, JRE_EXPECTED_MAJOR, JRE_ARCHIVE_SHA256,
      JRE_LIBJAVA_SHA256, JRE_LIBJVM_SHA256, JRE_PROFILE_FAMILY);
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
  int java_ok = !exception(env, "after call") && resumed == 913;
  printf("JRE_BRIDGE_RESULT=%s worker=%d proxy=%d java_resumed=%d\n",
         !worker && proxy && java_ok ? "PASS" : "FAIL", worker, proxy, java_ok);
  job->rc = !worker && proxy && java_ok ? 0 : 1;

detach:
  app_set_callback_context(NULL, NULL);
  if (cls) {
    (*env)->DeleteLocalRef(env, cls);
  }
  return job->rc;
}

#if JRE_USE_JLI
/* Startup model adapted from PojavLauncher
 * b12ad048157b3aa255d078c235dd4571e1900309:
 * JREUtils.initJavaRuntime/relocateLibPath and jre_launcher.c::launchJVM.
 * Test-only: preserve the JRE layout and use its JLI launcher. */
JNIEXPORT jint JNICALL Java_dev_glibc_worker_JreEntry_run(JNIEnv *env,
                                                          jclass cls) {
  (void)cls;
  JavaVM *vm = NULL;
  if (!launched_job || (*env)->GetJavaVM(env, &vm) != JNI_OK || !vm ||
      vm == launched_job->art) {
    return 1;
  }

  printf(
      "JRE_CREATE_RESULT=0 hotspot_vm=%p hotspot_env=%p art_vm=%p distinct=1 "
      "owner=libjli tid=%ld\n",
      (void *)vm, (void *)env, (void *)launched_job->art, syscall(SYS_gettid));

  return execute_bridge(launched_job, vm, env);
}

static int prepare_pojav(struct jre_job *job, const char *vm_path) {
  char vm_directory[PATH_MAX], search[PATH_MAX * 4], jli[PATH_MAX];
  snprintf(vm_directory, sizeof vm_directory, "%s", vm_path);
  char *slash = strrchr(vm_directory, '/');
  if (!slash) {
    return 1;
  }
  *slash = 0;

  snprintf(search, sizeof search, "%s:%s/%s/jli:%s/%s:%s", vm_directory,
           job->home, JRE_BOOT_LIBRARY_PATH, job->home, JRE_BOOT_LIBRARY_PATH,
           job->args[2]);
  if (setenv("LD_LIBRARY_PATH", search, 1)) {
    return 1;
  }

  void *libdl = dlopen("libdl.so", RTLD_NOW);
  void (*update)(const char *) =
      libdl ? (void *)dlsym(libdl, "android_update_LD_LIBRARY_PATH") : NULL;
  if (!update && libdl) {
    update = (void *)dlsym(libdl, "__loader_android_update_LD_LIBRARY_PATH");
  }
  if (update) {
    update(search);
  }
  printf("JRE_POJAV_LIBRARY_PATH updated=%d path=%s\n", update != NULL, search);
  if (libdl) {
    dlclose(libdl);
  }

  snprintf(jli, sizeof jli, "%s/%s/libjli.so", job->home,
           JRE_BOOT_LIBRARY_PATH);
  if (access(jli, R_OK)) {
    snprintf(jli, sizeof jli, "%s/%s/jli/libjli.so", job->home,
             JRE_BOOT_LIBRARY_PATH);
  }

  job->libjli = dlopen(jli, RTLD_NOW | RTLD_GLOBAL);
  if (!job->libjli) {
    printf("JRE_JLI_ERROR=%s\n", dlerror());
  }
  return !job->libjli;
}

static int preload_pojav(struct jre_job *job) {
  static const char *names[] = {"libverify.so", "libjava.so", "libnet.so",
                                "libnio.so"};

  for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s/%s", job->home, JRE_BOOT_LIBRARY_PATH,
             names[i]);
    if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) {
      printf("JRE_PRELOAD_ERROR library=%s error=%s\n", names[i], dlerror());
      return 1;
    }
  }

  return 0;
}

static int launch_jli(struct jre_job *job, JavaVMOption *options, int count) {
  char executable[PATH_MAX], host[PATH_MAX];
  if (count > 24) {
    return 1;
  }

  snprintf(executable, sizeof executable, "%s/bin/java", job->home);
  snprintf(host, sizeof host, "%s/libworker_host.so", job->args[2]);
  const char *argv[28] = {executable};
  for (int i = 0; i < count; i++) {
    argv[i + 1] = options[i].optionString;
  }
  argv[count + 1] = "dev.glibc.worker.JreEntry";
  argv[count + 2] = host;

  launched_job = job;
  jint rc = gw_test_pojav_jli(job->libjli, count + 3, argv);
  launched_job = NULL;

  JavaVM *listed = NULL;
  jsize remaining = -1;
  jint enum_rc = job->list_vms(&listed, 1, &remaining);
  printf("JRE_JLI_RESULT=%d owner=libjli\n", rc);
  printf("JRE_VM_ENUM stage=destroyed rc=%d count=%d\n", enum_rc, remaining);

  if (rc == 0 && enum_rc == JNI_OK && remaining == 0) {
    printf("JRE_DESTROY_RESULT=0 owner=libjli confirmed_by=vm-enumeration\n");
    return job->rc;
  }
  return 1;
}
#endif

static void *jre_thread(void *opaque) {
  struct jre_job *job = opaque;
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/%s", job->home, JRE_LIBJVM_PATH);
  setenv("JAVA_HOME", job->home, 1);
#if JRE_USE_JLI
  char probe_path[PATH_MAX], probe[4096];
  snprintf(probe_path, sizeof probe_path, "%s/launcher-probe.json", job->home);
  FILE *probe_file = fopen(probe_path, "r");
  if (!probe_file || !fgets(probe, sizeof probe, probe_file)) {
    if (probe_file) {
      fclose(probe_file);
    }
    return NULL;
  }
  fclose(probe_file);
  printf("JRE_EXEC_PROBE %s\n", probe);
#endif
#if JRE_USE_JLI
  if (prepare_pojav(job, path)) {
    return NULL;
  }
#endif
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
  job->list_vms = list_vms;
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
#if !JRE_USE_JLI
  if (gw_spike_set_jre_layout(&info, path)) {
    printf("JRE_LAYOUT_ADAPTER=FAIL\n");
    return NULL;
  }
#endif

#if JRE_USE_JLI
  if (preload_pojav(job)) {
    return NULL;
  }
#endif

  char home_option[PATH_MAX + 32], cp_option[PATH_MAX + 48],
      lib_option[PATH_MAX + 48], tmp_option[PATH_MAX + 48],
      search_option[PATH_MAX + 48];
  snprintf(home_option, sizeof home_option, "-Djava.home=%s", job->home);
  snprintf(cp_option, sizeof cp_option, "-Djava.class.path=%s/smoke.jar",
           job->home);
  snprintf(lib_option, sizeof lib_option, "-Dsun.boot.library.path=%s/%s",
           job->home, JRE_BOOT_LIBRARY_PATH);
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
#if JRE_EXPECTED_MAJOR >= 24
      {.optionString = "--enable-native-access=ALL-UNNAMED"},
#endif
      {.optionString = "-Xcheck:jni"}};
  JavaVMInitArgs init = {
      .version = JNI_VERSION_1_6,
      .nOptions = sizeof options / sizeof options[0] - (benchmark_jit ? 1 : 0),
      .options = options,
      .ignoreUnrecognized = JNI_FALSE};

  JavaVM *vm = NULL;
  JNIEnv *env = NULL;
  printf(
#if JRE_USE_JLI
      "JRE_STAGE=JLI_Launch checkjni=%s interpreter=%s "
#else
      "JRE_STAGE=JNI_CreateJavaVM checkjni=%s interpreter=%s "
#endif
      "benchmark_jit=%d\n",
      benchmark_jit ? "off" : "on", benchmark_jit ? "off" : "on",
      benchmark_jit);
#if JRE_USE_JLI
  (void)vm;
  (void)env;
  job->rc = launch_jli(job, options, init.nOptions);
  return NULL;
#else
  jint rc = create(&vm, (void **)&env, &init);
  printf(
      "JRE_CREATE_RESULT=%d hotspot_vm=%p hotspot_env=%p art_vm=%p distinct=%d "
      "tid=%ld\n",
      rc, (void *)vm, (void *)env, (void *)job->art, vm != job->art,
      syscall(SYS_gettid));
  if (rc != JNI_OK || !env || vm == job->art) {
    return NULL;
  }

  execute_bridge(job, vm, env);
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
#endif
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
