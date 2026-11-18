#define _GNU_SOURCE
#include "vm-start.h"
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include "jre_layout_spike.h"
#include "jre-layout.h"
#endif

int domain_vm_start(JavaVM **vm, JNIEnv **env, int checked) {
  const char *home = getenv("GW_JAVA_HOME");
  const char *library = getenv("GW_JVM_LIB");
  const char *jar = getenv("GW_DOMAIN_JAR");
  const char *boot = getenv("GW_JAVA_BOOT_LIB");
  if (!home || !library || !jar || !boot) {
    fprintf(stderr,
            "HotSpot fixture requires explicit home, library, jar and boot "
            "paths\n");
    return 1;
  }
  void *handle = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    fprintf(stderr, "HotSpot dlopen: %s\n", dlerror());
    return 1;
  }
  jint (*create)(JavaVM **, void **, void *) =
      dlsym(handle, "JNI_CreateJavaVM");
  if (!create) {
    return 1;
  }
#ifdef __ANDROID__
  /* The runner verifies this exact libjvm profile before deploying it. */
  Dl_info info = {0};
  char cached_path[PATH_MAX];
  if (!dladdr((void *)create, &info) ||
      snprintf(cached_path, sizeof cached_path, "%s/%s", home,
               JRE_LIBJVM_PATH) >= (int)sizeof cached_path ||
      gw_spike_set_jre_layout(&info, cached_path)) {
    return 1;
  }
#endif
  char home_arg[PATH_MAX], cp_arg[PATH_MAX], boot_arg[PATH_MAX];
  if (snprintf(home_arg, sizeof home_arg, "-Djava.home=%s", home) >=
          (int)sizeof home_arg ||
      snprintf(cp_arg, sizeof cp_arg, "-Djava.class.path=%s", jar) >=
          (int)sizeof cp_arg ||
      snprintf(boot_arg, sizeof boot_arg, "-Dsun.boot.library.path=%s", boot) >=
          (int)sizeof boot_arg) {
    return 1;
  }
  JavaVMOption options[] = {{.optionString = home_arg},
                            {.optionString = cp_arg},
                            {.optionString = boot_arg},
                            {.optionString = "-Djava.io.tmpdir=."},
                            {.optionString = "-Xms16m"},
                            {.optionString = "-Xmx128m"},
                            {.optionString = "-XX:+UseSerialGC"},
                            {.optionString = "-XX:-UsePerfData"},
                            {.optionString = "-XX:ActiveProcessorCount=2"},
                            {.optionString = "-Xcheck:jni"},
                            {.optionString = "-Xbatch"},
                            {.optionString = "-XX:-TieredCompilation"},
                            {.optionString = "-XX:CompileThreshold=100"},
                            {.optionString = "-XX:+PrintCompilation"}};
  JavaVMInitArgs init = {.version = JNI_VERSION_1_6,
                         .nOptions = sizeof options / sizeof options[0],
                         .options = options,
                         .ignoreUnrecognized = JNI_FALSE};
  if (!checked) {
    for (unsigned i = 0; i < sizeof options / sizeof options[0]; ++i) {
      if (!strcmp(options[i].optionString, "-Xcheck:jni") ||
          !strcmp(options[i].optionString, "-XX:+PrintCompilation")) {
        options[i].optionString = "-XX:-PrintCompilation";
      }
    }
  }
  jint status = create(vm, (void **)env, &init);
  printf("HOTSPOT_CREATE status=%d\n", status);
  fflush(stdout);
  if (status != JNI_OK) {
    return 1;
  }
  return 0;
}
