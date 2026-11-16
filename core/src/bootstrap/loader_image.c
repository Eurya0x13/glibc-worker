#define _GNU_SOURCE
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include "loader_image.h"
#include "arch.h"

enum { BOOTSTRAP_AUX_CAPACITY = 24 };

static void die(const char *msg) {
  perror(msg);
  exit(1);
}

static uintptr_t down(uintptr_t n, size_t a) {
  return n & ~(uintptr_t)(a - 1);
}

static uintptr_t up(uintptr_t n, size_t a) {
  return (n + a - 1) & ~(uintptr_t)(a - 1);
}

/* Bootstrap mapper for ONE pinned ld.so only. No dynamic section processing,
   relocations, symbol lookup, DSO loading or TLS implementation lives here. */
int gw_bootstrap_map_checked(const char *path, struct gw_bootstrap_image *out) {
  if (!path || !out) {
    return EINVAL;
  }
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return errno;
  }
  struct stat st;
  if (fstat(fd, &st)) {
    int error = errno;
    close(fd);
    return error;
  }
  void *mapping = MAP_FAILED;
  size_t mapped_size = 0;
  int error = ENOEXEC;

  Elf64_Ehdr e;
  if (!(pread(fd, &e, sizeof e, 0) == sizeof e)) {
    goto fail;
  }
  const int machine = GW_ELF_MACHINE;
  if (!(!memcmp(e.e_ident, ELFMAG, SELFMAG) &&
        e.e_ident[EI_CLASS] == ELFCLASS64 &&
        e.e_ident[EI_DATA] == ELFDATA2LSB && e.e_type == ET_DYN &&
        e.e_machine == machine && e.e_phentsize == sizeof(Elf64_Phdr) &&
        e.e_phnum && e.e_phnum <= 64)) {
    goto fail;
  }

  Elf64_Phdr ph[64];
  size_t bytes = e.e_phnum * sizeof *ph;
  if (!(e.e_phoff <= (uint64_t)st.st_size &&
        bytes <= (uint64_t)st.st_size - e.e_phoff)) {
    goto fail;
  }
  if (!(pread(fd, ph, bytes, e.e_phoff) == (ssize_t)bytes)) {
    goto fail;
  }

  size_t page = (size_t)sysconf(_SC_PAGESIZE), total = 0;
  uintptr_t last = 0;
  int entry_ok = 0, header_ok = 0;
  for (unsigned i = 0; i < e.e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
      Elf64_Phdr *p = &ph[i];
      if (!(p->p_filesz <= p->p_memsz && p->p_offset <= (uint64_t)st.st_size &&
            p->p_filesz <= (uint64_t)st.st_size - p->p_offset &&
            p->p_vaddr < 16 * 1024 * 1024 && p->p_memsz < 16 * 1024 * 1024)) {
        goto fail;
      }
      uintptr_t lo = down(p->p_vaddr, page),
                hi = up(p->p_vaddr + p->p_memsz, page);
      if (!(lo >= last)) {
        goto fail;
      }
      last = hi;
      if (hi > total) {
        total = hi;
      }
      if ((p->p_flags & PF_X) && e.e_entry >= p->p_vaddr &&
          e.e_entry < p->p_vaddr + p->p_memsz) {
        entry_ok = 1;
      }
      if (p->p_vaddr == 0 && p->p_offset == 0 &&
          p->p_filesz >= e.e_phoff + bytes) {
        header_ok = 1;
      }
    }
  }
  if (!(total && entry_ok && header_ok)) {
    goto fail;
  }

  char *base = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    error = errno;
    goto fail;
  }

  mapping = base;
  mapped_size = total;
  for (unsigned i = 0; i < e.e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
      Elf64_Phdr *p = &ph[i];
      uintptr_t lo = down(p->p_vaddr, page);
      size_t len = up(p->p_vaddr + p->p_memsz, page) - lo;
      if (mmap(base + lo, len, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        error = errno;
        goto fail;
      }
      if (!(pread(fd, base + p->p_vaddr, p->p_filesz, p->p_offset) ==
            (ssize_t)p->p_filesz)) {
        goto fail;
      }
      __builtin___clear_cache(base + lo, base + lo + len);
      int prot = ((p->p_flags & PF_R) ? PROT_READ : 0) |
                 ((p->p_flags & PF_W) ? PROT_WRITE : 0) |
                 ((p->p_flags & PF_X) ? PROT_EXEC : 0);
      if (!(!(p->p_flags & PF_W) || !(p->p_flags & PF_X))) {
        goto fail;
      }
      if (mprotect(base + lo, len, prot)) {
        error = errno;
        goto fail;
      }
    }
  }

  close(fd);
  *out = (struct gw_bootstrap_image){base, base + e.e_entry, total, e};
  return 0;
fail:
  if (mapping != MAP_FAILED) {
    munmap(mapping, mapped_size);
  }
  close(fd);
  return error;
}

struct gw_bootstrap_image gw_bootstrap_map(const char *path) {
  struct gw_bootstrap_image image;
  int error = gw_bootstrap_map_checked(path, &image);
  if (error) {
    errno = error;
    die("map loader");
  }
  return image;
}

/* Pushes s (with its NUL) down from the cursor and returns its address.
 * The cursor only decreases, so strings handed out earlier sit at higher
 * addresses and stay valid while later stack data is added below them. */
static uintptr_t copy_string(uintptr_t *cursor, const char *s) {
  size_t n = strlen(s) + 1;
  *cursor -= n;
  memcpy((void *)*cursor, s, n);
  return *cursor;
}

/* Validate every argument and the worst-case auxv/alignment budget before
   touching caller memory. String inputs must remain stable during the call. */
static int validate_stack(void *stack, size_t size, char **args, size_t argc) {
  if (!(stack && args && argc >= 1 && argc <= 16)) {
    return EINVAL;
  }
  if (!((uintptr_t)stack <= UINTPTR_MAX - size)) {
    return EINVAL;
  }

  const size_t reserved = 4096;
  const size_t words = 1 + argc + 1 + 2 + 1 + BOOTSTRAP_AUX_CAPACITY * 2;
  const size_t frame = words * sizeof(uintptr_t) + 16 + 15;
  if (!(size >= reserved && size - reserved >= frame)) {
    return EINVAL;
  }
  size_t remaining = size - reserved - frame;

  const char *fixed[] = {"LC_ALL=C", "GLIBC_TUNABLES=glibc.pthread.rseq=0",
                         GW_ARCH_PLATFORM};
  for (size_t i = 0; i < argc + 3; i++) {
    const char *text = i < argc ? args[i] : fixed[i - argc];
    if (!(text != NULL)) {
      return EINVAL;
    }
    size_t length = strnlen(text, remaining);
    if (!(length < remaining)) {
      return EINVAL;
    }
    remaining -= length + 1;
  }
  return 0;
}

/* Builds the initial frame top-down: strings, 16 AT_RANDOM bytes, then the
 * argc/argv/envp/auxv word block, ending at a 16-byte-aligned sp. The
 * stack region must be writable for its whole extent. Prevalidation
 * keeps at least 4 KiB of the region unused below sp, room for the
 * loader's earliest call frames. */
int gw_bootstrap_stack_checked(struct gw_bootstrap_image l, void *stack,
                               size_t size, char **args, size_t argc,
                               void **out) {
  if (!out || !l.base || !l.entry) {
    return EINVAL;
  }
  int error = validate_stack(stack, size, args, argc);
  if (error) {
    return error;
  }
  unsigned char entropy[16];
  size_t filled = 0;
  while (filled < sizeof entropy) {
    ssize_t nread = getrandom(entropy + filled, sizeof entropy - filled, 0);
    if (nread < 0 && errno == EINTR) {
      continue;
    }
    if (nread <= 0) {
      return nread < 0 ? errno : EIO;
    }
    filled += (size_t)nread;
  }
  uintptr_t cur = (uintptr_t)stack + size, av[16], env[3];

  for (size_t i = 0; i < argc; i++) {
    av[i] = copy_string(&cur, args[i]);
  }
  env[0] = copy_string(&cur, "LC_ALL=C");
  env[1] = copy_string(&cur, "GLIBC_TUNABLES=glibc.pthread.rseq=0");
  uintptr_t platform = copy_string(&cur, GW_ARCH_PLATFORM);

  cur -= 16;
  uintptr_t random = cur;
  memcpy((void *)random, entropy, sizeof entropy);

  Elf64_auxv_t aux[BOOTSTRAP_AUX_CAPACITY];
  size_t n = 0;
#define AUX(k, v) \
  (aux[n++] = (Elf64_auxv_t){.a_type = (k), .a_un.a_val = (uintptr_t)(v)})
  AUX(AT_PHDR, (char *)l.base + l.eh.e_phoff);
  AUX(AT_PHENT, sizeof(Elf64_Phdr));
  AUX(AT_PHNUM, l.eh.e_phnum);
  AUX(AT_ENTRY, l.entry);
  AUX(AT_BASE, 0);
  AUX(AT_PAGESZ, getauxval(AT_PAGESZ));
  AUX(AT_UID, getuid());
  AUX(AT_EUID, geteuid());
  AUX(AT_GID, getgid());
  AUX(AT_EGID, getegid());
  AUX(AT_SECURE, 0);
  AUX(AT_HWCAP, getauxval(AT_HWCAP));
  AUX(AT_HWCAP2, getauxval(AT_HWCAP2));
  AUX(AT_CLKTCK, getauxval(AT_CLKTCK));
  AUX(AT_SYSINFO_EHDR, getauxval(AT_SYSINFO_EHDR));
  AUX(AT_PLATFORM, platform);
  AUX(AT_RANDOM, random);
  AUX(AT_EXECFN, av[0]);
  AUX(AT_NULL, 0);
#undef AUX

  size_t words = 1 + argc + 1 + 2 + 1 + n * 2;
  uintptr_t *sp = (uintptr_t *)down(cur - words * sizeof(uintptr_t), 16),
            *w = sp;

  *w++ = argc;
  for (size_t i = 0; i < argc; i++) {
    *w++ = av[i];
  }
  *w++ = 0;
  *w++ = env[0];
  *w++ = env[1];
  *w++ = 0;
  memcpy(w, aux, n * sizeof *aux);

  *out = sp;
  return 0;
}

void *gw_bootstrap_stack(struct gw_bootstrap_image image, void *stack,
                         size_t size, char **args, size_t argc) {
  void *sp;
  int error = gw_bootstrap_stack_checked(image, stack, size, args, argc, &sp);
  if (error) {
    errno = error;
    die("build Guest stack");
  }
  return sp;
}
