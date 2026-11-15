#define _GNU_SOURCE
#include "signal-router.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef ROUTER_STUB
/* This contract tests late registration, not TLS restoration. */
void domain_signal_entry(int number, siginfo_t *info, void *context,
                         void *token) {
  signal_router_forward(number, info, context, token);
}
#else
static volatile sig_atomic_t hits, mask_seen;

static void handler(int number) {
  (void)number;
  sigset_t mask;
  sigprocmask(SIG_SETMASK, NULL, &mask);
  mask_seen = sigismember(&mask, 53);
  ++hits;
  errno = EIO;
}

#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      fprintf(stderr, "late signal failure line=%d errno=%d\n", __LINE__, \
              errno);                                                     \
      return 1;                                                           \
    }                                                                     \
    ++checks;                                                             \
  } while (0)

int main(int argc, char **argv) {
  int checks = 0;
  CHECK(argc == 2);
  struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART};
  sigaddset(&action.sa_mask, 53);
  CHECK(!sigaction(52, &action, NULL));
  struct sigaction baseline;
  CHECK(!sigaction(52, NULL, &baseline));
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(library);
  int (*init)(signal_router_backend) =
      dlsym(library, "signal_router_init_with_backend");
  int (*adopt)(void) = dlsym(library, "signal_router_adopt");
  int (*restore)(void) = dlsym(library, "signal_router_restore");
  int (*verify)(void) = dlsym(library, "signal_router_verify");
  signal_router_backend route = dlsym(library, "signal_router_action");
  CHECK(init && adopt && restore && verify && route);
  CHECK(init(NULL) == ENOSYS);
  CHECK(!init(sigaction));
  CHECK(init(sigaction) == EALREADY);
  CHECK(!adopt());
  CHECK(adopt() == EBUSY);
  struct sigaction logical, physical;
  CHECK(!route(52, NULL, &logical));
  CHECK(logical.sa_handler == handler &&
        logical.sa_flags == baseline.sa_flags &&
        sigismember(&logical.sa_mask, 53));
  CHECK(!sigaction(52, NULL, &physical));
  CHECK(physical.sa_handler != handler);
  errno = E2BIG;
  CHECK(!raise(52));
  CHECK(hits == 1 && mask_seen && errno == E2BIG);
  CHECK(!verify());
  /* A bypassing writer must be detected, including flags/mask-only changes. */
  CHECK(!sigaction(52, &baseline, NULL));
  CHECK(verify() == EBUSY && restore() == EBUSY);
  CHECK(!sigaction(52, &physical, NULL));
  CHECK(!verify());
  action.sa_flags = SA_RESETHAND;
  CHECK(!route(52, &action, &logical) && logical.sa_handler == handler);
  CHECK(!raise(52) && hits == 2);
  CHECK(!route(52, NULL, &logical) && logical.sa_handler == SIG_DFL);
  /* ART probes kernel flag support with SA_UNSUPPORTED. The logical query
   * must not report request bits that the kernel cleared. */
  action.sa_flags = 0x400 | 0x800 | 0x02000000;
  CHECK(!route(52, &action, NULL));
  CHECK(!route(52, NULL, &logical) && !sigaction(52, NULL, &physical));
  CHECK((logical.sa_flags & action.sa_flags) ==
        (physical.sa_flags & action.sa_flags));
  CHECK(!restore());
  CHECK(!sigaction(52, NULL, &physical) && physical.sa_handler == handler);
  /* A fresh init cannot inherit a stale disposition from the previous run. */
  CHECK(!init(sigaction));
  action = baseline;
  sigaddset(&action.sa_mask, 51);
  CHECK(!sigaction(52, &action, NULL));
  CHECK(adopt() == EBUSY && restore() == EBUSY);
  CHECK(!sigaction(52, &baseline, NULL));
  CHECK(!adopt() && !restore());
  printf(
      "{\"result\":\"PASS\",\"checks\":%d,\"scope\":\"late Host signal "
      "routing\"}\n",
      checks);
  return 0;
}
#endif
