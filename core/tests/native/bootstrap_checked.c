#define _GNU_SOURCE
#include "loader_image.h"
#include "arch.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned file_descriptors(void) {
  DIR *dir = opendir("/proc/self/fd");
  assert(dir);
  unsigned count = 0;
  while (readdir(dir)) {
    ++count;
  }
  assert(!closedir(dir));
  return count;
}

int main(void) {
  struct gw_bootstrap_image image, untouched;
  memset(&image, 0x5a, sizeof image);
  untouched = image;
  assert(gw_bootstrap_map_checked(NULL, &image) == EINVAL);
  assert(gw_bootstrap_map_checked("/gw-no-such-loader", &image) == ENOENT);
  assert(!memcmp(&image, &untouched, sizeof image));
  char filename[] = "/tmp/gw-bootstrap-checked-XXXXXX";
  int fd = mkstemp(filename);
  assert(fd >= 0);
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  Elf64_Ehdr eh = {.e_type = ET_DYN,
                   .e_machine = GW_ELF_MACHINE,
                   .e_entry = 256,
                   .e_phoff = sizeof eh,
                   .e_phentsize = sizeof(Elf64_Phdr),
                   .e_phnum = 2};
  memcpy(eh.e_ident, ELFMAG, SELFMAG);
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  Elf64_Phdr segments[] = {{.p_type = PT_LOAD,
                            .p_flags = PF_R | PF_X,
                            .p_filesz = 512,
                            .p_memsz = page},
                           {.p_type = PT_LOAD,
                            .p_flags = PF_R | PF_W | PF_X,
                            .p_vaddr = page,
                            .p_offset = 512,
                            .p_filesz = 64,
                            .p_memsz = page}};
  assert(!ftruncate(fd, 1024));
  assert(pwrite(fd, &eh, sizeof eh, 0) == sizeof eh);
  assert(pwrite(fd, segments, sizeof segments, sizeof eh) == sizeof segments);
  unsigned before = file_descriptors();
  for (int i = 0; i < 1000; ++i) {
    /* The invalid second segment is encountered after reserving/mapping the
     * first. Repeated failure must close descriptors and keep out unchanged. */
    assert(gw_bootstrap_map_checked(filename, &image) == ENOEXEC);
    assert(!memcmp(&image, &untouched, sizeof image));
  }
  assert(file_descriptors() == before);
  segments[1].p_flags = PF_R | PF_W;
  assert(pwrite(fd, segments, sizeof segments, sizeof eh) == sizeof segments);
  assert(!gw_bootstrap_map_checked(filename, &image));
  assert(image.size == 2 * page);
  assert(image.entry == (char *)image.base + eh.e_entry);
  assert(!munmap(image.base, image.size));
  assert(!close(fd));
  assert(!unlink(filename));

  unsigned char stack[16384];
  memset(stack, 0x5a, sizeof stack);
  void *sp = (void *)1;
  char *args[] = {NULL};
  assert(gw_bootstrap_stack_checked(image, stack, sizeof stack, args, 1, &sp) ==
         EINVAL);
  assert(sp == (void *)1);
  for (size_t i = 0; i < sizeof stack; ++i) {
    assert(stack[i] == 0x5a);
  }
  puts("{\"result\":\"PASS\",\"failed_maps\":1000,\"checked_errors\":true}");
  return 0;
}
