#define _GNU_SOURCE
#include "domain.h"
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static __thread int guest_counter = 41;
static domain_probe probe;

static int execute(struct exchange *e);

static int callback(struct exchange *e, int value) {
  e->value = value;
  e->state = DOMAIN_CALLBACK;
  e->swap(&e->guest, &e->host);
  /* A Host callback may recursively enter this suspended Guest stack. */
  while (e->state == DOMAIN_CALL) {
    execute(e);
    e->swap(&e->guest, &e->host);
  }
  CHECK(e, e->state == DOMAIN_DONE);
  return e->value;
}

static int execute(struct exchange *e) {
  int expected = guest_counter;
  int result = probe(e, callback);
  CHECK(e, guest_counter == expected && errno == EBADF);
  e->value = result;
  e->state = DOMAIN_DONE;
  return result;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    _exit(91);
  }
  struct exchange *e = (void *)(uintptr_t)strtoull(argv[1], NULL, 16);
  e->guest_tid = syscall(SYS_gettid);
  e->guest_tls = (uintptr_t)&guest_counter;
  CHECK(e, guest_counter == 41 && fesetround(FE_UPWARD) == 0);
  errno = EBADF;
  /* The Host-installed diagnostic handler deliberately uses no libc/TLS.
   * It records the inherited Guest TP; it is not a signal-routing solution. */
  CHECK(e, raise(SIGUSR1) == 0);
  e->state = DOMAIN_READY;
  e->swap(&e->guest, &e->host);

  void *handle = dlopen(e->dso, RTLD_NOW | RTLD_LOCAL);
  if (!handle || !(probe = (domain_probe)dlsym(handle, "domain_probe_call"))) {
    _exit(92);
  }
  /* No dlclose/TCB teardown: retained mappings are part of this disposable
   * bootstrap-only experiment, not a production ownership contract. */
  for (;;) {
    CHECK(e, e->state == DOMAIN_CALL);
    execute(e);
    e->swap(&e->guest, &e->host);
  }
}
