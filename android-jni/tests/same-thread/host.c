#define _GNU_SOURCE
#include "domain_runtime.h"
#include "vm-start.h"
#include "signal_bootstrap.h"
#ifdef __ANDROID__
#include "bootstrap-check.h"
#endif

static pthread_barrier_t loaded, finished;
static JavaVM *vm;
static jclass bench_class;
static jmethodID run_method;
static const char *module_path;
static int measuring, threads;
static _Atomic int failures;
static const struct gw_loader_api_v1 *loader;
static struct gw_loader_library library;
extern _Atomic int domain_sr_deliveries;

static int exception(JNIEnv *env) {
  if (!(*env)->ExceptionCheck(env)) {
    return 0;
  }
  (*env)->ExceptionDescribe(env);
  (*env)->ExceptionClear(env);
  return 1;
}

static void inject_context_failure(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  atomic_store(&fail_next_context, 1);
}

static jint vm_sampling_signals(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  return atomic_load_explicit(&domain_sr_deliveries, memory_order_relaxed);
}

static void conformance(JNIEnv *env) {
  const char *path = getenv("GW_CONFORMANCE");
  REQUIRE(path);
  struct gw_loader_library fixture;
  struct gw_loader_error error = {0};
  REQUIRE(loader->open(env, path, 0, &fixture, &error) == GW_OK &&
          !exception(env));
  if (getenv("GW_FATAL") && !strcmp(getenv("GW_FATAL"), "1")) {
    jclass module = (*env)->FindClass(env, "conformance/Module");
    REQUIRE(module && !exception(env));
    jmethodID group = (*env)->GetStaticMethodID(env, module, "group", "(I[B)I");
    REQUIRE(group && !exception(env));
    (*env)->CallStaticIntMethod(env, module, group, 99, NULL);
    REQUIRE(0); /* FatalError must not return. */
  }
  jclass cls = (*env)->FindClass(env, "DomainConformance");
  REQUIRE(cls && !exception(env));
  JNINativeMethod injection = {"failNextContext", "()V",
                               (void *)inject_context_failure};
  REQUIRE((*env)->RegisterNatives(env, cls, &injection, 1) == JNI_OK &&
          !exception(env));
  jmethodID run = (*env)->GetStaticMethodID(env, cls, "run", "()I");
  REQUIRE(run && !exception(env));
  jint result = (*env)->CallStaticIntMethod(env, cls, run);
  REQUIRE(!exception(env) && result == 1);
  (*env)->DeleteLocalRef(env, cls);
  /* Java Thread.join observes VM termination before pthread-key destructors
   * necessarily finish. Wait for native context reclamation before dlclose. */
  for (int n = 0; n < 5000; ++n) {
    if (session.domain
            ? atomic_load(&domains_created) ==
                  atomic_load(&domains_finished) + 1
            : atomic_load(&gw_host_bridge->live_guest_threads) == 1) {
      break;
    }
    usleep(1000);
  }
  REQUIRE(session.domain
              ? atomic_load(&domains_created) ==
                    atomic_load(&domains_finished) + 1
              : atomic_load(&gw_host_bridge->live_guest_threads) == 1);
  REQUIRE(loader->close(env, fixture.handle, &error) == GW_OK &&
          !exception(env));
}

static void *carrier(void *opaque) {
  int index = (int)(intptr_t)opaque;
  JNIEnv *env;
  REQUIRE((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK);
  if (!index) {
    if (!measuring) {
      conformance(env);
    }
    struct gw_loader_error error = {0};
    int rc = loader->open(env, module_path, 0, &library, &error);
    if (rc != GW_OK) {
      fprintf(stderr, "JNI_LOAD_ERROR %d %s\n", rc, error.message);
    }
    REQUIRE(rc == GW_OK && !exception(env));
  }
  pthread_barrier_wait(&loaded);
  jint result = (*env)->CallStaticIntMethod(env, bench_class, run_method, index,
                                            (jboolean)measuring);
  if (exception(env) || result != 1) {
    atomic_fetch_add(&failures, 1);
  }
  pthread_barrier_wait(&finished);
  if (!index) {
    struct gw_loader_error error = {0};
    REQUIRE(loader->close(env, library.handle, &error) == GW_OK &&
            !exception(env));
  }
  pthread_barrier_wait(&finished);
  extern int gw_domain_release_context(void);
  REQUIRE(!gw_domain_release_context());
  REQUIRE((*vm)->DetachCurrentThread(vm) == JNI_OK);
  return NULL;
}

#ifdef GW_EXPERIMENT_SYSTEM_LOAD
extern int domain_system_load_run(JNIEnv *);

static void *system_load_carrier(void *unused) {
  (void)unused;
  JNIEnv *env;
  REQUIRE((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK);
  REQUIRE(domain_system_load_run(env));
  extern int gw_domain_release_context(void);
  REQUIRE(!gw_domain_release_context());
  REQUIRE((*vm)->DetachCurrentThread(vm) == JNI_OK);
  return NULL;
}
#endif

int main(int argc, char **argv) {
  REQUIRE(argc == 5);
  const char *mode = getenv("GW_DOMAIN_MODE");
  int selected = jni_domain_select(mode);
  if (selected < 0) {
    fputs("JNI_DOMAIN_REJECT invalid_backend\n", stderr);
    return 2;
  }
  session.domain = selected;
  printf("JNI_DOMAIN_SELECTION {\"requested\":\"%s\",\"backend\":\"%s\"}\n",
         mode ? mode : "default", session.domain ? "domain" : "worker");
  mode = session.domain ? "domain" : "worker";
  measuring = getenv("GW_BENCH") && !strcmp(getenv("GW_BENCH"), "1");
  threads = getenv("GW_THREADS") ? atoi(getenv("GW_THREADS")) : 1;
  REQUIRE(threads > 0 && threads <= 64);
  REQUIRE(!gw_host_signal_bootstrap());
#ifdef __ANDROID__
  check_native_bootstrap("before-vm");
#endif
  JNIEnv *env;
  REQUIRE(!domain_vm_start(&vm, &env, !measuring));
#ifdef __ANDROID__
  check_native_bootstrap("after-vm");
  if (!measuring) {
    check_bionic_reregistration();
  }
#endif
  module_path = argv[4];
  char *directory = strdup(module_path);
  REQUIRE(directory && strrchr(directory, '/'));
  *strrchr(directory, '/') = 0;
  session.shared.magic = MAGIC;
  struct gw_host_bridge_config config = {
      .abi_version = GW_HOST_BRIDGE_CONFIG_V2,
      .struct_size = sizeof config,
      .flags = GW_HOST_DIRECT_IMAGES,
      .native_directory = directory,
      .image_catalog = directory};
#ifdef GW_EXPERIMENT_SYSTEM_LOAD
  char java_library[4096];
  REQUIRE(snprintf(java_library, sizeof java_library, "%s/libjava.so",
                   getenv("GW_JAVA_BOOT_LIB")) < (int)sizeof java_library);
  config.flags |= GW_HOST_VM_ADAPTER;
  config.jre_library_path = java_library;
#endif
  int started = full_host_start(&session.shared, env, &config);
  int pending = exception(env);
  REQUIRE(started == JNI_OK && !pending);
  struct gw_signal_contract signals;
  REQUIRE(!gw_host_signal_contract_init(&signals));
  session.reserved_signals = signals.host_reserved;
  session.note_guest = signal_note_guest_thread;
  session.guest_thread = signal_guest_lifecycle;
  start_owner(argv);
  REQUIRE(full_host_ready() == JNI_OK);
  loader = gw_loader_query(GW_LOADER_ABI_V1, sizeof *loader);
  REQUIRE(loader);
#ifdef GW_EXPERIMENT_SYSTEM_LOAD
  pthread_t integration;
  REQUIRE(!pthread_create(&integration, NULL, system_load_carrier, NULL));
  REQUIRE(!pthread_join(integration, NULL));
  REQUIRE(!atomic_load(&gw_host_bridge->active_modules));
  REQUIRE(!atomic_load(&gw_host_bridge->live_closures));
  /* VM cleanup threads can remain alive. Like the worker lifecycle contract,
   * their idle bindings stay pinned until process disposal, never freed on a
   * foreign thread. No claim of full runtime reclamation is made. */
  REQUIRE(!atomic_load(&gw_host_bridge->errors));
#ifdef __ANDROID__
  check_native_bootstrap("after-guest-jni");
#endif
  if (!measuring && session.domain && getenv("GW_BOOTSTRAP_SIGNAL_PRESSURE")) {
    extern _Atomic int domain_sr_guest_deliveries;
    int sr = atomic_load(&domain_sr_deliveries);
    int guest_sr = atomic_load(&domain_sr_guest_deliveries);

    REQUIRE(guest_sr >= 3 && sr >= guest_sr);
    printf("NATIVE_SIGNAL_DELIVERIES {\"sr\":%d,\"sr_guest\":%d}\n", sr,
           guest_sr);
  }
  REQUIRE(full_host_shutdown() == JNI_OK);
  REQUIRE(!owner_command(NULL, JNI_DOMAIN_STOP));
  REQUIRE((*vm)->DestroyJavaVM(vm) == JNI_OK);
  REQUIRE(!gw_host_signal_restore());
  printf(
      "SYSTEM_LOAD_LIFECYCLE "
      "{\"result\":\"PASS\",\"domains_created\":%d,\"domains_finished\":%d,"
      "\"idle_domains_retained\":%d,\"vm_destroyed\":true}\n",
      atomic_load(&domains_created), atomic_load(&domains_finished),
      atomic_load(&domains_created) - atomic_load(&domains_finished));
  if (getenv("GW_INLINE_VM")) {
    puts("APP_DOMAIN_EXIT=0");
  }
  fflush(NULL);
  _exit(0);
#endif
  jclass local = (*env)->FindClass(env, "JniDomainProbe");
  REQUIRE(local && !exception(env));
  JNINativeMethod sampler = {"vmSamplingSignals", "()I",
                             (void *)vm_sampling_signals};
  REQUIRE((*env)->RegisterNatives(env, local, &sampler, 1) == JNI_OK &&
          !exception(env));
  bench_class = (*env)->NewGlobalRef(env, local);
  (*env)->DeleteLocalRef(env, local);
  run_method = (*env)->GetStaticMethodID(env, bench_class, "run", "(IZ)I");
  REQUIRE(run_method && !exception(env));
  REQUIRE(!pthread_barrier_init(&loaded, NULL, threads));
  REQUIRE(!pthread_barrier_init(&finished, NULL, threads));
  pthread_t *tasks = calloc((size_t)threads, sizeof *tasks);
  REQUIRE(tasks);
  for (int i = 0; i < threads; ++i) {
    REQUIRE(!pthread_create(&tasks[i], NULL, carrier, (void *)(intptr_t)i));
  }
  for (int i = 0; i < threads; ++i) {
    REQUIRE(!pthread_join(tasks[i], NULL));
  }
  free(tasks);
  REQUIRE(atomic_load(&domains_created) == atomic_load(&domains_finished));
  jfieldID unloaded =
      (*env)->GetStaticFieldID(env, bench_class, "unloads", "I");
  REQUIRE(unloaded &&
          (*env)->GetStaticIntField(env, bench_class, unloaded) == 1 &&
          !exception(env));
  REQUIRE(!owner_command(NULL, JNI_DOMAIN_STOP));
  REQUIRE(atomic_load(&gw_host_bridge->loads) == (measuring ? 1 : 2) &&
          atomic_load(&gw_host_bridge->image_closes) == (measuring ? 1 : 2));
  if (!measuring) {
    unsigned covered = 0;
    for (int i = 0; i < 235; ++i) {
      covered += atomic_load(&gw_host_bridge->hits[i]) != 0;
    }
    printf(
        "JNI_DOMAIN_COVERAGE "
        "{\"apis\":%u,\"contexts\":%d,\"domains_created\":%d,\"domains_"
        "finished\":%d,\"sr_deliveries\":%d}\n",
        covered, fb_context_capacity(gw_host_bridge),
        atomic_load(&domains_created), atomic_load(&domains_finished),
        atomic_load(&domain_sr_deliveries));
  }
  REQUIRE(atomic_load(&gw_host_bridge->errors) ==
          atomic_load(&expected_errors));
  REQUIRE(full_host_shutdown() == JNI_OK);
  (*env)->DeleteGlobalRef(env, bench_class);
  REQUIRE((*vm)->DestroyJavaVM(vm) == JNI_OK);
  REQUIRE(!gw_host_signal_restore());
  printf(
      "JNI_DOMAIN_RESULT "
      "{\"result\":\"%s\",\"backend\":\"%s\",\"threads\":%d,\"benchmark\":%d,"
      "\"errors\":%d}\n",
      atomic_load(&failures) ? "FAIL" : "PASS", mode, threads, measuring,
      atomic_load(&failures));
  fflush(NULL);
  _exit(!!atomic_load(&failures));
}
