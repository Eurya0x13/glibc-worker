#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "loader_image.h"

static char *region;
static size_t size;
static struct gw_bootstrap_image image;

static void invalid(unsigned test) {
  char *args[17];
  for (size_t i = 0; i < 17; i++) {
    args[i] = "loader";
  }

  void *stack = region;
  size_t bytes = size, count = 1;
  char **input = args;

  char large[32768];
  memset(large, 'x', sizeof large - 1);
  large[sizeof large - 1] = 0;

  switch (test) {
    case 0:
      count = 0;
      break;
    case 1:
      count = 17;
      break;
    case 2:
      count = SIZE_MAX;
      break;
    case 3:
      input = NULL;
      break;
    case 4:
      args[0] = NULL;
      break;
    case 5:
      bytes = 4096;
      break;
    case 6:
      args[0] = large;
      break;
    case 7:
      stack = (void *)(UINTPTR_MAX - 32);
      bytes = 64;
      break;
    case 8:
      stack = NULL;
      break;
    case 9:
      bytes = 0;
      break;
    default:
      abort();
  }

  gw_bootstrap_stack(image, stack, bytes, input, count);
  _exit(99);
}

int main(void) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  size = page * 4;
  char *mapping =
      mmap(NULL, size + 2 * page, PROT_NONE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  assert(mapping != MAP_FAILED);
  region = mapping + page;
  assert(!mprotect(region, size, PROT_READ | PROT_WRITE));

  for (unsigned test = 0; test < 10; test++) {
    memset(region, 0x5a, size);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
      assert(freopen("/dev/null", "w", stderr));
      invalid(test);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    for (size_t j = 0; j < size; j++) {
      assert((unsigned char)region[j] == 0x5a);
    }
  }

  image.base = (void *)(uintptr_t)0x100000;
  image.entry = (void *)(uintptr_t)0x101000;
  image.eh.e_phoff = 64;
  image.eh.e_phnum = 7;
  for (size_t count = 1; count <= 16; count += 15) {
    char *args[16];
    for (size_t i = 0; i < count; i++) {
      args[i] = i ? "--guest" : "ld.so";
    }

    uintptr_t *sp = gw_bootstrap_stack(image, region, size - 7, args, count);
    assert(!((uintptr_t)sp & 15) && (char *)sp >= region + 4096);
    assert(*sp++ == count);
    for (size_t i = 0; i < count; i++) {
      assert(!strcmp((char *)*sp++, args[i]));
    }
    assert(!*sp++);
    assert(!strcmp((char *)*sp++, "LC_ALL=C"));
    assert(!strcmp((char *)*sp++, "GLIBC_TUNABLES=glibc.pthread.rseq=0"));
    assert(!*sp++);

    Elf64_auxv_t *aux = (void *)sp;
    int execfn = 0, random = 0, phdr = 0;
    for (size_t i = 0; aux[i].a_type; i++) {
      assert(i < 24);
      uintptr_t value = aux[i].a_un.a_val;
      if (aux[i].a_type == AT_EXECFN) {
        execfn = !strcmp((char *)value, "ld.so");
      }
      if (aux[i].a_type == AT_RANDOM) {
        random = value >= (uintptr_t)region &&
                 value + 16 <= (uintptr_t)region + size;
      }
      if (aux[i].a_type == AT_PHDR) {
        phdr = value == 0x100040;
      }
    }
    assert(execfn && random && phdr);
  }

  assert(!munmap(mapping, size + 2 * page));
  puts(
      "{\"result\":\"PASS\",\"invalid_no_write_cases\":10,\"valid_argc\":[1,16]"
      ",\"guard_pages\":true}");
}
