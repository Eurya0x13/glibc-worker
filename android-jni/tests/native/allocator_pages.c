/* Android-only controlled allocator experiment. Options affect this short-lived
 * process, never an application. mincore observes the old address after free;
 * no freed memory is dereferenced. Retaining a mapping is distinct from keeping
 * its physical pages resident. The experiment observes that distinction rather
 * than inferring residency from repeated malloc addresses.
 * MapAllocatorCache::store and SCUDO_MALLOPT(M_DECAY_TIME):
 * https://android.googlesource.com/platform/external/scudo/+/8e2d762272cd7bed1362cbe22016d1d9754d74f8/standalone/secondary.h
 * https://android.googlesource.com/platform/external/scudo/+/8e2d762272cd7bed1362cbe22016d1d9754d74f8/standalone/wrappers_c.inc
 */
#define _GNU_SOURCE
#ifndef __ANDROID__
#error "This allocator diagnostic requires Android bionic"
#endif
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static uint64_t nanos(void) {
  struct timespec t;
  assert(!clock_gettime(CLOCK_MONOTONIC, &t));
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int main(int argc, char **argv) {
  const size_t bytes = argc == 2 ? strtoul(argv[1], NULL, 10) : 1048576;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  assert(bytes >= page && bytes <= 8 * 1048576 && bytes % page == 0);
  unsigned char *source = malloc(bytes);
  assert(source);
  memset(source, 0x6b, bytes);
  volatile unsigned checksum = 0;
  for (int state = 0; state < 4; ++state) {
    int decay = (state == 0 || state == 3) ? 1 : 0;
    assert(mallopt(M_DECAY_TIME, decay));
    for (int retain = 0; retain < 2; ++retain) {
      unsigned char *held = retain ? malloc(bytes) : NULL;
      if (held) {
        memset(held, 1, bytes);
      }
      struct rusage before, after;
      assert(!getrusage(RUSAGE_SELF, &before));
      uint64_t start = nanos();
      uintptr_t last_address = 0;
      for (int i = 0; i < 256; ++i) {
        unsigned char *p = retain ? held : malloc(bytes);
        assert(p);
        memcpy(p, source, bytes);
        checksum += p[i];
        last_address = (uintptr_t)p;
        if (!retain) {
          free(p);
        }
      }
      uint64_t elapsed = nanos() - start;
      assert(!getrusage(RUSAGE_SELF, &after));
      uintptr_t base = last_address & ~(uintptr_t)(page - 1);
      size_t count = (last_address - base + bytes + page - 1) / page;
      unsigned char residency[count];
      int status = mincore((void *)base, count * page, residency);
      int error = status ? errno : 0;
      size_t resident = 0;
      if (!status) {
        for (size_t i = 0; i < count; ++i) {
          resident += residency[i] & 1;
        }
      }
      printf(
          "{\"state\":%d,\"decay\":%d,\"retained\":%d,\"bytes\":%zu,"
          "\"page_bytes\":%zu,\"ns_per_call\":%.3f,\"minor_faults_per_call\":%."
          "3f,"
          "\"queried_pages\":%zu,\"resident_pages\":%zu,\"mincore_errno\":%d}"
          "\n",
          state, decay, retain, bytes, page, (double)elapsed / 256,
          (double)(after.ru_minflt - before.ru_minflt) / 256, count, resident,
          error);
      free(held);
    }
  }
  free(source);
  return checksum == 0;
}
