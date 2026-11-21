#define _GNU_SOURCE
#include "signal_bootstrap.h"
#include "signal-router.h"
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <string.h>

static int loaded_vm(struct dl_phdr_info *image, size_t size, void *unused) {
  (void)size;
  (void)unused;
  const char *name = strrchr(image->dlpi_name, '/');
  name = name ? name + 1 : image->dlpi_name;
  return !strcmp(name, "libart.so") || !strcmp(name, "libartd.so") ||
         !strcmp(name, "libsigchain.so") || !strcmp(name, "libjvm.so");
}

static int registration_interposed(void) {
  Dl_info executable;
  if (!dladdr((void *)gw_host_signal_bootstrap, &executable)) {
    return 0;
  }

  /* The executable's definitions must win in the Host global symbol scope.
   * A local DSO that exports identically named functions cannot catch the
   * registrations of subsequently loaded VM and platform libraries. */
  const char *names[] = {
      "sigaction",
      "signal",
#ifdef __ANDROID__
      "sigaction64",
#endif
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
    Dl_info owner;
    void *symbol = dlsym(RTLD_DEFAULT, names[i]);
    if (!symbol || !dladdr(symbol, &owner) ||
        owner.dli_fbase != executable.dli_fbase) {
      return 0;
    }
  }
  return 1;
}

int gw_host_signal_bootstrap(void) {
  if (signal_router_error()) {
    return EIO;
  }
  if (signal_router_enabled()) {
    return EALREADY;
  }
  if (dl_iterate_phdr(loaded_vm, NULL) || !registration_interposed()) {
    return ENOTSUP;
  }

  int error = signal_router_init();
  if (error) {
    return error;
  }
  error = signal_router_adopt();
  if (!error) {
    error = signal_router_guest_ready();
  }
  if (error && signal_router_restore()) {
    return signal_router_quarantine();
  }
  return error;
}

int gw_host_signal_contract_init(struct gw_signal_contract *contract) {
  if (!contract) {
    return EINVAL;
  }

  int error = signal_router_guest_ready();
  if (error) {
    return error;
  }
  *contract =
      (struct gw_signal_contract){.version = GW_SIGNAL_CONTRACT_V1,
                                  .size = sizeof *contract,
                                  .host_reserved = signal_router_reserved()};
  return 0;
}

int gw_host_signal_restore(void) {
  if (!signal_router_enabled()) {
    return signal_router_error() ? EIO : EINVAL;
  }
  return signal_router_restore();
}
