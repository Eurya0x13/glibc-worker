#include "abi.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "mutex_contract.h"
static __thread int counter = 17;

int guest_probe(struct packet *p, callback_t cb) {
  volatile int *q = malloc(sizeof(*q));
  if (!q) {
    return -1;
  }
  *q = p->value + 5;
  p->value = *q;
  free((void *)q);
  errno = 0;

  if (close(-1) != -1 || errno != EBADF) {
    return -2;
  }
  if (counter != 17) {
    return -3;
  }
  counter = 29;

#if defined(__aarch64__)
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(p->guest_tp));
#else
  p->guest_tp = 0;
#endif
  p->callback_value = cb(p);

  if (errno != EBADF || counter != 29) {
    return -4;
  }
  int saved = errno;
  int mutexes = gw_mutex_contract(GW_MUTEX_REJECTED);
  errno = saved;
  if (mutexes) {
    return -6;
  }
  return p->value == 42 && p->callback_value == 84 ? 0 : -5;
}
