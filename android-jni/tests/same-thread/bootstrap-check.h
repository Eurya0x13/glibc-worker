#ifndef GW_FIXTURE_NATIVE_BOOTSTRAP_CHECK_H
#define GW_FIXTURE_NATIVE_BOOTSTRAP_CHECK_H

#include <dlfcn.h>
#include <link.h>
#include <string.h>
#include <unistd.h>

static int find_art(struct dl_phdr_info *info, size_t size, void *unused) {
  (void)size;
  (void)unused;
  const char *base = strrchr(info->dlpi_name, '/');
  const char *name = base ? base + 1 : info->dlpi_name;
  return !strcmp(name, "libart.so") || !strcmp(name, "libartd.so") ||
         !strcmp(name, "libsigchain.so") ||
         !strcmp(name, "libandroid_runtime.so");
}

static void check_native_bootstrap(const char *stage) {
  REQUIRE(!dl_iterate_phdr(find_art, NULL));
  struct gw_signal_contract contract;
  REQUIRE(!gw_host_signal_contract_init(&contract));
  printf(
      "NATIVE_BOOTSTRAP {\"stage\":\"%s\",\"pid\":%d,\"uid\":%d,"
      "\"art_loaded\":false,\"router_verified\":true}\n",
      stage, (int)getpid(), (int)getuid());
  fflush(stdout);
}

static _Thread_local volatile unsigned private_tls = 0x1937;
static _Atomic unsigned private_deliveries;

static int observe_bionic_action(void *opaque) {
  REQUIRE(opaque == &private_deliveries);
  REQUIRE(private_tls == 0x1937);
  atomic_fetch_add_explicit(&private_deliveries, 1, memory_order_relaxed);
  return 1;
}

static void check_bionic_reregistration(void) {
  int (*run_on_all)(int (*)(void *), void *) =
      dlsym(RTLD_NEXT, "android_run_on_all_threads");
  REQUIRE(run_on_all);
  REQUIRE(run_on_all(observe_bionic_action, &private_deliveries));
  unsigned deliveries =
      atomic_load_explicit(&private_deliveries, memory_order_relaxed);
  struct gw_signal_contract contract;
  REQUIRE(deliveries > 1 && !gw_host_signal_contract_init(&contract));
  printf(
      "NATIVE_BIONIC_REGISTRATION {\"result\":\"PASS\","
      "\"deliveries\":%u,\"host_tls\":true}\n",
      deliveries);
  fflush(stdout);
}

#endif
