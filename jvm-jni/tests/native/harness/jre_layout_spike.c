#define _GNU_SOURCE
#include "jre_layout_spike.h"
#include "jre-layout.h"
#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <string.h>

/* Disposable test-app startup layout only. Pojav already starts its own JRE. */
struct cache_check {
  uintptr_t base, address;
  int writable;
};

static int check_cache_mapping(struct dl_phdr_info *info, size_t size,
                               void *opaque) {
  (void)size;
  struct cache_check *c = opaque;
  if (info->dlpi_addr != c->base) {
    return 0;
  }

  for (unsigned i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *p = &info->dlpi_phdr[i];
    uintptr_t start = c->base + p->p_vaddr, end = start + p->p_memsz;
    if (p->p_type == PT_LOAD && (p->p_flags & PF_W) && !(p->p_flags & PF_X) &&
        c->address >= start && c->address + JRE_PATH_CACHE_SIZE <= end) {
      c->writable = 1;
    }
  }
  return 1;
}

int gw_spike_set_jre_layout(Dl_info *info, const char *path) {
  struct cache_check c = {
      .base = (uintptr_t)info->dli_fbase,
      .address = (uintptr_t)info->dli_fbase + JRE_PATH_CACHE_OFFSET};
  dl_iterate_phdr(check_cache_mapping, &c);
  if (!c.writable || strlen(path) >= JRE_PATH_CACHE_SIZE) {
    return 1;
  }

  char *cache = (char *)c.address;
  for (unsigned i = 0; i < JRE_PATH_CACHE_SIZE; i++) {
    if (cache[i]) {
      return 1;
    }
  }
  strcpy(cache, path);

  printf(
      "JRE_LAYOUT_ADAPTER=private-path-cache offset=0x%lx sha256=%s path=%s\n",
      JRE_PATH_CACHE_OFFSET, JRE_LIBJVM_SHA256, path);
  return 0;
}
