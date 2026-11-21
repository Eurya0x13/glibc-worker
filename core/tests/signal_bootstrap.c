#define _GNU_SOURCE
#include "signal_bootstrap.h"
#include "signal-router.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>

static int before_main;
static volatile sig_atomic_t delivered;
static struct sigaction inherited;

static void handle_usr1(int number) {
  delivered += number == SIGUSR1;
}

__attribute__((constructor)) static void native_constructor(void) {
  struct sigaction ignored = {.sa_handler = SIG_IGN}, previous, actual;
  errno = EDOM;
  assert(!sigaction(SIGUSR1, &ignored, &previous) && errno == EDOM);
  assert(!sigaction(SIGUSR1, NULL, &actual) && actual.sa_handler == SIG_IGN);
  assert(!sigaction(SIGUSR1, &previous, NULL));
  struct sigaction handler = {.sa_handler = handle_usr1,
                              .sa_flags = SA_RESTART};
  sigemptyset(&handler.sa_mask);
  assert(!sigaction(SIGUSR1, &handler, &inherited));
  before_main = 1;
}

int main(int argc, char **argv) {
  assert(before_main && !signal_router_enabled());
  assert(argc == 2);
  int (*native)(int, const struct sigaction *, struct sigaction *) =
      dlsym(RTLD_NEXT, "sigaction");
  assert(native);

  struct gw_signal_contract contract = {.host_reserved = 17};
  assert(gw_host_signal_restore() == EINVAL);
  assert(gw_host_signal_contract_init(&contract) == EINVAL);
  assert(!contract.version && !contract.size && contract.host_reserved == 17);
  void *foreign = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  assert(foreign && gw_host_signal_bootstrap() == ENOTSUP);
  assert(!signal_router_enabled());
  assert(!dlclose(foreign));
  struct sigaction actual;
  assert(!native(SIGUSR1, NULL, &actual) && actual.sa_handler == handle_usr1);

  assert(!gw_host_signal_bootstrap());
  assert(gw_host_signal_bootstrap() == EALREADY);
  assert(!gw_host_signal_contract_init(&contract));
  assert(!gw_signal_contract_validate(&contract));
  assert(contract.host_reserved & (UINT64_C(1) << (SIGUSR2 - 1)));
  assert(!sigaction(SIGUSR1, NULL, &actual) &&
         actual.sa_handler == handle_usr1);
  assert(!native(SIGUSR1, NULL, &actual) && actual.sa_handler != handle_usr1);
  assert(!raise(SIGUSR1) && delivered == 1);

  assert(!gw_host_signal_restore());
  assert(!signal_router_enabled());
  assert(!native(SIGUSR1, NULL, &actual) && actual.sa_handler == handle_usr1);
  assert(!sigaction(SIGUSR1, &inherited, NULL));
  puts(
      "{\"result\":\"PASS\",\"native_before_bootstrap\":true,"
      "\"reject_foreign_chain\":true,\"inherited_action_restored\":true}");
}
