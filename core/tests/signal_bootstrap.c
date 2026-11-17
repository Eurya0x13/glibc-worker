#define _GNU_SOURCE
#include "signal-router.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>

static int before_main;

__attribute__((constructor)) static void native_constructor(void) {
  struct sigaction ignored = {.sa_handler = SIG_IGN}, previous, actual;
  errno = EDOM;
  assert(!sigaction(SIGUSR1, &ignored, &previous) && errno == EDOM);
  assert(!sigaction(SIGUSR1, NULL, &actual) && actual.sa_handler == SIG_IGN);
  assert(!sigaction(SIGUSR1, &previous, NULL));
  before_main = 1;
}

int main(void) {
  assert(before_main && !signal_router_enabled());
  assert(!signal_router_init());
  assert(!signal_router_adopt());
  assert(!signal_router_guest_ready());
  assert(!signal_router_restore());
  puts("{\"result\":\"PASS\",\"native_before_bootstrap\":true}");
}
