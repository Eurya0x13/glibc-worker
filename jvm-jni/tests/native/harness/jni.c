#define _GNU_SOURCE
#include <jni.h>
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include "full_bridge.h"
#include "arch.h"
#ifndef GLIBC_RUNTIME_LABEL
#define GLIBC_RUNTIME_LABEL "unspecified"
#endif
#ifndef GUEST_JNI_MODE_LABEL
#define GUEST_JNI_MODE_LABEL "normal"
#endif
extern int worker_host_main(int argc, char **argv);
extern int install_sigsys_diagnostic(int fd);
extern int app_host_jni_begin(JNIEnv *env);
extern int app_host_jni_finish(void);
extern int run_android_jre(const char *home, char **args, JavaVM *art);
/* The callback target is HotSpot, assigned on its bionic Host thread.
   The incoming ART environment is only used for Android argument/report IO. */
static JNIEnv *host_env;
static jclass host_class;
static int callbacks;
static __thread uint64_t selected_cpu_mask;

/* Test-only restriction, installed by the actual app thread before it creates
   HotSpot or Guest children. run-as cannot set another SELinux domain's mask.
 */
JNIEXPORT void JNICALL Java_dev_glibc_worker_NativeBridge_restrictCpuMask(
    JNIEnv *env, jclass cls, jlong mask) {
  (void)cls;
  cpu_set_t wanted, actual;
  CPU_ZERO(&wanted);
  for (unsigned i = 0; i < 64; i++) {
    if ((uint64_t)mask & (UINT64_C(1) << i)) {
      CPU_SET(i, &wanted);
    }
  }

  const char *error = NULL;
  if (!mask) {
    error = "Empty test CPU mask";
  } else if (sched_setaffinity(0, sizeof wanted, &wanted) ||
             sched_getaffinity(0, sizeof actual, &actual)) {
    error = strerror(errno);
  } else if (!CPU_EQUAL(&wanted, &actual)) {
    error = "Requested test CPUs are unavailable";
  }
  if (error) {
    jclass exception =
        (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (exception) {
      (*env)->ThrowNew(env, exception, error);
    }
    return;
  }

  selected_cpu_mask = (uint64_t)mask;
}

void app_set_callback_context(JNIEnv *env, jclass cls) {
  host_env = env;
  host_class = cls;
}

int app_host_callback(int value) {
  int saved_errno = errno;
  jmethodID method =
      (*host_env)->GetStaticMethodID(host_env, host_class, "callback", "(I)I");

  int result = -84;
  if (method) {
    result =
        (*host_env)->CallStaticIntMethod(host_env, host_class, method, value);
  }

  if ((*host_env)->ExceptionCheck(host_env)) {
    (*host_env)->ExceptionDescribe(host_env);
    (*host_env)->ExceptionClear(host_env);
    result = -84;
  } else {
    callbacks++;
  }

  errno = saved_errno;
  return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  __android_log_print(ANDROID_LOG_INFO, "GlibcWorker",
                      "HOST_JNI_ONLOAD pid=%d uid=%d", getpid(), getuid());
  (void)vm;
  return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL Java_dev_glibc_worker_NativeBridge_run(
    JNIEnv *env, jclass cls, jstring dir, jstring report, jstring jre_home,
    jboolean smoke_only) {
  (void)cls;
  const char *d = (*env)->GetStringUTFChars(env, dir, NULL);
  if (!d) {
    return NULL;
  }
  const char *r = (*env)->GetStringUTFChars(env, report, NULL);
  if (!r) {
    (*env)->ReleaseStringUTFChars(env, dir, d);
    return NULL;
  }
  int fd = open(r, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) {
    (*env)->ReleaseStringUTFChars(env, report, r);
    (*env)->ReleaseStringUTFChars(env, dir, d);
    return (*env)->NewStringUTF(env,
                                "FAIL: cannot open application-private report");
  }

  int oldout = dup(STDOUT_FILENO), olderr = dup(STDERR_FILENO);
  if (oldout < 0 || olderr < 0 || dup2(fd, STDOUT_FILENO) < 0 ||
      dup2(fd, STDERR_FILENO) < 0) {
    _exit(80); /* Isolated probe process; never continue with half redirected
                  IO. */
  }
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);
  if (install_sigsys_diagnostic(fd)) {
    _exit(81);
  }

  printf("APP_CONTEXT pid=%d uid=%d native_arch=%s\n", getpid(), getuid(),
         GW_ARCH_NAME);
  if (selected_cpu_mask) {
    printf("TEST_CPU_AFFINITY mask=%llx scope=host-and-descendants\n",
           (unsigned long long)selected_cpu_mask);
  }
  printf("RUNTIME_VARIANT=%s\n", GLIBC_RUNTIME_LABEL);
  printf("GUEST_JNI_TEST_MODE=%s\n", GUEST_JNI_MODE_LABEL);
  printf("nativeLibraryDir=%s\n", d);
  FILE *ctx = fopen("/proc/self/attr/current", "r");
  if (ctx) {
    char b[512] = {0};
    if (fgets(b, sizeof b, ctx)) {
      printf("SELinux domain=%s\n", b);
    }
    fclose(ctx);
  }
  FILE *enforce = fopen("/sys/fs/selinux/enforce", "r");
  if (enforce) {
    printf("SELinux enforce=%c\n", fgetc(enforce));
    fclose(enforce);
  }

  const char *home = (*env)->GetStringUTFChars(env, jre_home, NULL);
  if (!home) {
    _exit(83);
  }
  JavaVM *art_vm = NULL;
  if ((*env)->GetJavaVM(env, &art_vm) != JNI_OK) {
    _exit(84);
  }
  printf("JNI_TARGET=Android-JRE-HotSpot\n");
  char loader[PATH_MAX], worker[PATH_MAX], guest[PATH_MAX], guest_jni[PATH_MAX];
  snprintf(loader, sizeof loader, "%s/libguest_loader.so", d);
  snprintf(worker, sizeof worker, "%s/libguest_worker.so", d);
  snprintf(guest, sizeof guest, "%s/libguest_probe.so", d);
  snprintf(guest_jni, sizeof guest_jni, "%s/libguest_jni.so", d);
  char *args[] = {smoke_only ? "smoke" : "coverage",
                  loader,
                  (char *)d,
                  worker,
                  guest,
                  guest_jni,
                  (char *)home};

  int rc = 1;
  int jni_good = 0;
  printf("TEST_GROUP=%s\n", smoke_only ? "smoke" : "coverage");
  if (getuid() >= 10000) {
    rc = run_android_jre(home, args, art_vm);
    jni_good = !rc;
  } else {
    printf("FAIL: expected ordinary application UID\n");
  }

  printf("HOST_JNI_RETURN native_rc=%d java_callbacks=%d pid=%d uid=%d\n", rc,
         callbacks, getpid(), getuid());
  const char *status_label =
      !rc && callbacks == 1 && jni_good ? "PASS" : "FAIL";
  printf("APP_RESULT=%s\n", status_label);
  fflush(stdout);
  fflush(stderr);

  dup2(oldout, STDOUT_FILENO);
  dup2(olderr, STDERR_FILENO);
  close(oldout);
  close(olderr);
  host_env = NULL;
  host_class = NULL;

  /* Keep the ResultReceiver parcel bounded, and never silently cut off the
     final APP_RESULT or the added original-JAR assertion inventory. */
  struct stat report_stat = {0};
  const size_t limit = 256 * 1024;
  char *text = NULL;
  const char *failure = "APP_RESULT=FAIL\nCannot read complete native report\n";
  if (!fstat(fd, &report_stat) && report_stat.st_size >= 0 &&
      (uint64_t)report_stat.st_size <= limit && lseek(fd, 0, SEEK_SET) == 0) {
    size_t length = (size_t)report_stat.st_size, offset = 0;
    text = calloc(length + 1, 1);
    while (text && offset < length) {
      ssize_t n = read(fd, text + offset, length - offset);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      offset += (size_t)n;
    }
    if (offset != length) {
      free(text);
      text = NULL;
    }
  } else if (report_stat.st_size > (off_t)limit) {
    failure =
        "APP_RESULT=FAIL\nNative report exceeds 256 KiB; retain "
        "native-report.txt\n";
  }
  close(fd);

  (*env)->ReleaseStringUTFChars(env, report, r);
  (*env)->ReleaseStringUTFChars(env, dir, d);
  (*env)->ReleaseStringUTFChars(env, jre_home, home);
  jstring result = (*env)->NewStringUTF(env, text ? text : failure);
  free(text);
  __android_log_print(ANDROID_LOG_INFO, "GlibcWorker",
                      "HOST_JNI_RETURN rc=%d callbacks=%d", rc, callbacks);
  return result;
}
