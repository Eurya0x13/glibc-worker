#define _GNU_SOURCE
#include "domain.h"
#include <errno.h>
#include <fenv.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Loaded after the Guest has first yielded: exercises dynamically added TLS. */
static __thread int dso_counter = 23;

int domain_probe_call(struct exchange *e, domain_callback callback) {
  int local;
  CHECK(e,
        (uintptr_t)&local >= e->stack_low && (uintptr_t)&local < e->stack_high);
  CHECK(e, syscall(SYS_gettid) == e->host_tid);
  CHECK(e, fegetround() == FE_UPWARD);
  e->dso_tls = (uintptr_t)&dso_counter;
  CHECK(e, dso_counter == 23 + e->entries);
  ++dso_counter;
  ++e->entries;
  int *allocation = malloc(4096);
  if (!allocation) {
    ++e->failures;
    return -1;
  }
  allocation[0] = e->value + 5;
  e->value = allocation[0];
  int *previous_allocation = e->allocation;
  e->allocation = allocation;
  errno = 0;
  CHECK(e, close(-1) == -1 && errno == EBADF);
  int result = callback(e, e->value);
  CHECK(e, errno == EBADF && fegetround() == FE_UPWARD);
  /* Nested calls legitimately change this thread's single TLS instance. */
  CHECK(e, dso_counter == 23 + e->entries);
  CHECK(e, e->allocation == allocation && allocation[0] == 42);
  e->allocation = previous_allocation;
  free(allocation);
  ++e->calls;
  return result;
}
