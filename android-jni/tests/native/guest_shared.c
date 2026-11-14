#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
static _Atomic int count;
static __thread int tls = 9;
static _Atomic int *constructed, *destroyed;
static void *memory;

__attribute__((constructor)) static void init(void) {
  memory = malloc(32);
}

__attribute__((destructor)) static void fini(void) {
  free(memory);
  if (destroyed) {
    atomic_fetch_add(destroyed, 1);
  }
}

void multi_dep_observe(_Atomic int *c, _Atomic int *d) {
  if (!constructed) {
    constructed = c;
    destroyed = d;
    atomic_fetch_add(c, 1);
  }
}

int multi_dep_add(int n) {
  return atomic_fetch_add(&count, n) + n;
}

int multi_dep_value(void) {
  return atomic_load(&count);
}

uintptr_t multi_dep_cookie(void) {
  return (uintptr_t)&count;
}

int multi_dep_tls(int n) {
  int old = tls;
  if (n >= 0) {
    tls = n;
  }
  return old;
}
