#define _GNU_SOURCE
#include "vm-start.h"
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef GW_INLINE_VM
static struct sigaction outer_previous;

static void outer_handler(int number, siginfo_t *info, void *context) {
  outer_previous.sa_sigaction(number, info, context);
}

/* Reproduce a launcher library wrapping the existing ART chain at the kernel
 * boundary. Installing the domain router must not move this wrapper below ART:
 * it already forwards to ART, so doing so creates a cycle. */
static int install_outer_chain(void) {
  void *libc = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
  int (*action)(int, const struct sigaction *, struct sigaction *) =
      libc ? dlsym(libc, "sigaction") : NULL;
  if (!action || action(SIGSEGV, NULL, &outer_previous) ||
      !(outer_previous.sa_flags & SA_SIGINFO) ||
      outer_previous.sa_handler == SIG_DFL ||
      outer_previous.sa_handler == SIG_IGN) {
    return -1;
  }
  struct sigaction outer = outer_previous;
  outer.sa_sigaction = outer_handler;
  if (action(SIGSEGV, &outer, NULL)) {
    return -1;
  }
  puts("LATE_OUTER_CHAIN installed=1 forwards_existing_art=1");
  fflush(stdout);
  return 0;
}
#endif

static void receive_reclaim_test(int number) {
  (void)number;
}

static int check_reclaim(JavaVM *vm, JNIEnv *env, void *bridge) {
  int (*install)(JNIEnv *) = dlsym(bridge, "domain_late_signal_install");
  int (*enable)(int) = dlsym(bridge, "domain_late_signal_reclaim_enable");
  int (*collect)(unsigned, unsigned *) =
      dlsym(bridge, "domain_late_signal_reclaim");
  int (*registration)(int, const struct sigaction *, struct sigaction *) =
      dlsym(bridge, "signal_router_action");
  int (*restore)(void) = dlsym(bridge, "domain_late_signal_restore");
  if (!install || !enable || !collect || !registration || !restore) {
    return 2;
  }

  unsigned reclaimed = 0;
  if (collect(1000, &reclaimed) != ENOTSUP || install(env) || enable(47)) {
    return 3;
  }
  struct sigaction action = {.sa_handler = receive_reclaim_test,
                             .sa_flags = SA_RESTART};
  uint64_t mask = UINT64_MAX;
  sigemptyset(&action.sa_mask);
  memcpy(&action.sa_mask, &mask, sizeof mask);
  if (registration(48, &action, NULL) || registration(48, &action, NULL)) {
    return 3;
  }

  int result = collect(1000, &reclaimed);
  if (result || reclaimed != 1) {
    fprintf(stderr, "Late VM reclaim: status=%d reclaimed=%u\n", result,
            reclaimed);
    return 3;
  }
  if ((*env)->ExceptionCheck(env) || (*vm)->DestroyJavaVM(vm) != JNI_OK ||
      restore()) {
    return 3;
  }
  puts(
      "LATE_SIGNAL_RECLAIM {\"result\":\"PASS\","
      "\"reclaimed\":1,\"vm_destroyed\":true}");
  return 0;
}

/* Deliberately exports no sigaction interposer. Start the real VM first,
 * then load the bridge DSO, matching the important premain ordering. */
int main(int argc, char **argv) {
  JavaVM *vm;
  JNIEnv *env;
  int measuring = getenv("GW_BENCH") && !strcmp(getenv("GW_BENCH"), "1");
  if (domain_vm_start(&vm, &env, !measuring)) {
    return 1;
  }
#ifdef GW_INLINE_VM
  if (install_outer_chain()) {
    return 4;
  }
#endif
  const char *path = getenv("GW_LATE_BRIDGE");
  void *bridge = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
  if (!bridge) {
    fprintf(stderr, "late bridge dlopen: %s\n", dlerror());
    return 2;
  }
  if (getenv("GW_LATE_RECLAIM_TEST")) {
    return check_reclaim(vm, env, bridge);
  }
  const char *expected_status = getenv("GW_LATE_EXPECT_STATUS");
  if (getenv("GW_LATE_FAIL_AFTER") || expected_status) {
    int (*install)(JNIEnv *) = dlsym(bridge, "domain_late_signal_install");
    int status = install ? install(env) : EINVAL;
    int expected = expected_status ? atoi(expected_status) : ENOMEM;
    int (*enabled)(void) = dlsym(bridge, "signal_router_enabled");
    if ((*env)->ExceptionCheck(env) || status != expected || !enabled ||
        enabled() || (*vm)->DestroyJavaVM(vm) != JNI_OK) {
      return 3;
    }
    puts("LATE_SIGNAL_REJECTION {\"result\":\"PASS\",\"vm_destroyed\":true}");
    fflush(stdout);
    return 0;
  }
  int (*run)(JavaVM *, JNIEnv *, int, char **) =
      dlsym(bridge, "domain_run_existing_vm");
  if (!run) {
    fprintf(stderr, "late bridge entry: %s\n", dlerror());
    return 2;
  }
  return run(vm, env, argc, argv);
}

#ifdef GW_INLINE_VM
JNIEXPORT void JNICALL
Java_dev_glibc_worker_domains_hotspot_MainActivity_runInline(
    JNIEnv *art, jclass cls, jobjectArray arguments, jstring report) {
  (void)cls;
  jsize count = (*art)->GetArrayLength(art, arguments);
  if (count != 5) {
    return;
  }
  char *argv[6] = {0};
  for (int i = 0; i < count; ++i) {
    jstring value = (*art)->GetObjectArrayElement(art, arguments, i);
    const char *text =
        value ? (*art)->GetStringUTFChars(art, value, NULL) : NULL;
    if (!text) {
      return;
    }
    argv[i] = strdup(text);
    (*art)->ReleaseStringUTFChars(art, value, text);
    (*art)->DeleteLocalRef(art, value);
    if (!argv[i]) {
      return;
    }
  }
  const char *path = (*art)->GetStringUTFChars(art, report, NULL);
  if (!path) {
    return;
  }
  char *directory = strdup(path);
  char *separator = directory ? strrchr(directory, '/') : NULL;
  if (!separator) {
    (*art)->ReleaseStringUTFChars(art, report, path);
    free(directory);
    return;
  }
  *separator = 0;
  int changed = chdir(directory);
  free(directory);
  if (changed) {
    (*art)->ReleaseStringUTFChars(art, report, path);
    return;
  }
  FILE *output = freopen(path, "w", stdout);
  (*art)->ReleaseStringUTFChars(art, report, path);
  if (!output || dup2(fileno(stdout), STDERR_FILENO) < 0) {
    return;
  }
  setenv("GW_INLINE_VM", "1", 1);
  printf("APP_DOMAIN_SUPERVISOR_UID=%u\nAPP_DOMAIN_INLINE=1\n", getuid());
  fflush(stdout);
  int status = main(count, argv);
  printf("APP_DOMAIN_EXIT=%d\n", status);
  fflush(NULL);
  _exit(status);
}
#endif
