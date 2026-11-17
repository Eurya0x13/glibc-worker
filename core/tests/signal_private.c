#define _GNU_SOURCE
#include "signal-router.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>

int main(void) {
  signal_router_backend native = dlsym(RTLD_NEXT, "sigaction");
  assert(native);
  assert(signal_router_lease_guest_nptl() == EINVAL);
  struct sigaction original, network_original,
      ignored = {.sa_handler = SIG_IGN};
  assert(!native(63, NULL, &original));
  assert(!native(63, &ignored, NULL));
  assert(!signal_router_init());
  assert(signal_router_lease_guest_nptl() == EBUSY);
  assert(!signal_router_restore());
  assert(!native(63, &original, NULL));

  assert(!native(62, &ignored, &network_original));
  assert(!signal_router_init());
  assert(signal_router_reserved() & (UINT64_C(1) << 61));
  assert(!signal_router_lease_guest_nptl());
  assert(!signal_router_lease_guest_nptl());
  for (int number = 63; number <= 64; ++number) {
    struct sigaction previous;
    assert(sigaction(number, &ignored, &previous) == -1 && errno == EBUSY);
    assert(sigaction(number, NULL, &previous) == -1 && errno == EBUSY);
    /* Owning Guest libc uses its native kernel entry, not this Host API. */
    assert(!native(number, &ignored, NULL));
  }
  assert(!signal_router_verify());
  assert(!signal_router_restore());
  struct sigaction network;
  assert(!native(62, NULL, &network) && network.sa_handler == SIG_IGN);

  for (int number = 63; number <= 64; ++number) {
    struct sigaction actual;
    assert(sigaction(number, NULL, &actual) == -1 && errno == EBUSY);
    assert(!native(number, NULL, &actual) && actual.sa_handler == SIG_IGN);
  }
  assert(!signal_router_init());
  assert(!signal_router_verify());
  assert(!signal_router_restore());
  assert(!native(62, &network_original, NULL));
  puts("{\"result\":\"PASS\",\"permanent_private_lease\":true}");
}
