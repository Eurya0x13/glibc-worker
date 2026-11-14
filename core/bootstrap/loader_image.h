#ifndef GW_BOOTSTRAP_LOADER_IMAGE_H
#define GW_BOOTSTRAP_LOADER_IMAGE_H
#include <elf.h>
#include <stddef.h>

/* One validated, pinned ld.so mapping held for the process lifetime. base is
 * where ELF vaddr 0 landed; entry is the absolute entry (base + e_entry); size
 * is the page-rounded reserved span. eh is saved so consumers can rebuild
 * auxv/PHDR data later without rereading the file. */
struct gw_bootstrap_image {
  void *base, *entry;
  size_t size;
  Elf64_Ehdr eh;
};

/* Maps the pinned glibc dynamic loader, a single ET_DYN image checked against
 * GW_ELF_MACHINE, into fresh private memory. Any failure exits the process:
 * there is no error return and no caller able to recover from a half-built
 * Guest. On success the fd is closed and the caller owns the mapping. */
struct gw_bootstrap_image gw_bootstrap_map(const char *);

/* Builds the raw SysV initial process stack (argv, fixed env, synthesized
 * auxv, AT_RANDOM) that the clone child's ld.so consumes directly; that thread
 * has no C runtime, so the result must not depend on libc TLS. argc must be
 * 1..16 to fit the fixed argv array (av[0] doubles as AT_EXECFN). The
 * environment is deliberately fixed: LC_ALL=C and glibc rseq disabled. */
void *gw_bootstrap_stack(struct gw_bootstrap_image, void *, size_t, char **,
                         size_t);
#endif
