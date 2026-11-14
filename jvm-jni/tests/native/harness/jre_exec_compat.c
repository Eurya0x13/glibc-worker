/* Test launcher subprocesses do not inherit the app manifest's heap-tagging
 * opt-out. Old Android HotSpot builds encode raw heap addresses in generated
 * instructions and cannot accept tagged allocator pointers. Keep this setting
 * local to the exec probe/baseline; never preload this into ART or Guest. */
#include <android/api-level.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>

__attribute__((constructor)) static void configure_jre_subprocess(void) {
#if defined(__aarch64__)
  if (android_get_device_api_level() >= 31) {
    if (!mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE)) {
      fputs("JRE_EXEC_COMPAT_ERROR=heap tagging opt-out refused\n", stderr);
      _exit(125);
    }

    fputs("JRE_EXEC_COMPAT heap_tagging=disabled\n", stderr);
  }
#endif
}
