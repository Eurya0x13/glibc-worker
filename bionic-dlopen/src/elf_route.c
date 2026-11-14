#include "elf_route.h"
#include "arch.h"
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Hard classifier limits. Anything at or past a limit fails with
   GW_E_CAPACITY -- never "unmarked" -- so oversized metadata cannot
   silently fall through to the platform loader. */
enum {
  MAX_PHDRS = 128,
  MAX_DYNAMIC = 512,
  MAX_NAMES = 256,
  MAX_VERNEEDS = 64,
  MAX_VERSIONS = 256
};

/* Bounded metadata window over an open fd: only program headers stay
   resident, everything else is read on demand. The fd lives for a single
   classify call. Every failure routes out through error. */
struct image {
  int fd;
  uint64_t size;
  Elf64_Phdr ph[MAX_PHDRS];
  unsigned count;
  struct gw_loader_error *error;
};

/* Snapshot of the dynamic table plus the VA anchors the scans need.
   Zeroed strings/versions fields mean the image carries no such metadata,
   which is a valid observation, not an error. */
struct dynamic {
  Elf64_Dyn entries[MAX_DYNAMIC];
  size_t count;
  uint64_t strings, strings_size, versions, version_count;
};

/* Single failure path: record the code and message, return 0; every parser
   propagates that 0 outward unchanged. */
static int fail(struct image *image, int code, const char *message) {
  if (image->error) {
    image->error->code = code;
    snprintf(image->error->message, sizeof image->error->message, "%s",
             message);
  }
  return 0;
}

/* Reads only fully inside [0, file size]; a short or out-of-range read is
   a truncated-image failure, never a partial result. */
static int read_file(struct image *image, uint64_t offset, void *out,
                     size_t size) {
  if (offset > image->size || size > image->size - offset ||
      pread(image->fd, out, size, (off_t)offset) != (ssize_t)size) {
    return fail(image, GW_E_IMAGE, "ELF metadata is truncated or unreadable");
  }
  return 1;
}

/* Translates a virtual address through file-backed PT_LOAD segments only;
   anything landing in bss or outside the mappings is an image error, since
   classification never touches runtime memory. */
static int read_va(struct image *image, uint64_t address, void *out,
                   size_t size) {
  for (unsigned j = 0; j < image->count; j++) {
    const Elf64_Phdr *p = &image->ph[j];
    if (p->p_type == PT_LOAD && address >= p->p_vaddr &&
        address - p->p_vaddr <= p->p_filesz &&
        size <= p->p_filesz - (address - p->p_vaddr) &&
        p->p_offset <= UINT64_MAX - (address - p->p_vaddr)) {
      return read_file(image, p->p_offset + (address - p->p_vaddr), out, size);
    }
  }

  return fail(image, GW_E_IMAGE,
              "ELF metadata address is outside file-backed segments");
}

/* Fetches a NUL-terminated name by string-table index, capped at
   MAX_NAMES bytes: an overlong name is GW_E_CAPACITY, a missing
   terminator GW_E_IMAGE. */
static int text_at(struct image *image, const struct dynamic *dynamic,
                   uint64_t index, char out[MAX_NAMES]) {
  if (!dynamic->strings || !dynamic->strings_size ||
      index >= dynamic->strings_size || dynamic->strings > UINT64_MAX - index) {
    return fail(image, GW_E_IMAGE, "Invalid ELF string table index");
  }

  uint64_t available = dynamic->strings_size - index;
  size_t size = available < MAX_NAMES ? (size_t)available : MAX_NAMES;
  if (!read_va(image, dynamic->strings + index, out, size)) {
    return 0;
  }

  if (!memchr(out, 0, size)) {
    return fail(image, available > size ? GW_E_CAPACITY : GW_E_IMAGE,
                available > size ? "ELF metadata name exceeds classifier limit"
                                 : "ELF metadata name is not terminated");
  }
  return 1;
}

/* Accepts only ELF64 little-endian ET_DYN for the target machine with
   exact-sized headers; every mismatch is GW_E_IMAGE, an oversized phnum
   is GW_E_CAPACITY. */
static int read_headers(struct image *image) {
  struct stat st;
  if (fstat(image->fd, &st) || st.st_size < 0 || !S_ISREG(st.st_mode)) {
    return fail(image, GW_E_IMAGE, "ELF image is not a readable regular file");
  }
  image->size = (uint64_t)st.st_size;

  Elf64_Ehdr header;
  if (!read_file(image, 0, &header, sizeof header)) {
    return 0;
  }

  if (memcmp(header.e_ident, ELFMAG, SELFMAG) ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB ||
      header.e_ident[EI_VERSION] != EV_CURRENT ||
      header.e_version != EV_CURRENT || header.e_type != ET_DYN ||
      header.e_machine != GW_ELF_MACHINE || header.e_ehsize != sizeof header ||
      header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum) {
    return fail(image, GW_E_IMAGE, "Unsupported or invalid ELF header");
  }

  if (header.e_phnum > MAX_PHDRS) {
    return fail(image, GW_E_CAPACITY,
                "ELF program headers exceed classifier limit");
  }
  image->count = header.e_phnum;
  return read_file(image, header.e_phoff, image->ph,
                   image->count * sizeof *image->ph);
}

/* Loads the single PT_DYNAMIC plus the DT_* anchors the scans need. A
   dynamic-less image is valid (nothing to inspect); duplicate, misaligned
   or unterminated tables are image errors. DT_NULL truncates the count. */
static int read_dynamic(struct image *image, struct dynamic *dynamic) {
  for (unsigned j = 0; j < image->count; j++) {
    const Elf64_Phdr *p = &image->ph[j];
    if (p->p_type != PT_DYNAMIC) {
      continue;
    }
    if (dynamic->count || !p->p_filesz || p->p_filesz % sizeof(Elf64_Dyn)) {
      return fail(image, GW_E_IMAGE, "Invalid or duplicate ELF dynamic table");
    }
    uint64_t count = p->p_filesz / sizeof(Elf64_Dyn);
    if (count > MAX_DYNAMIC) {
      return fail(image, GW_E_CAPACITY,
                  "ELF dynamic table exceeds classifier limit");
    }
    if (!read_file(image, p->p_offset, dynamic->entries, (size_t)p->p_filesz)) {
      return 0;
    }
    dynamic->count = (size_t)count;
  }

  if (!dynamic->count) {
    return 1;  // Valid image without dynamic dependencies.
  }

  for (size_t j = 0; j < dynamic->count; j++) {
    const Elf64_Dyn *entry = &dynamic->entries[j];
    switch (entry->d_tag) {
      case DT_NULL:
        dynamic->count = j;
        return 1;
      case DT_STRTAB:
        dynamic->strings = entry->d_un.d_ptr;
        break;
      case DT_STRSZ:
        dynamic->strings_size = entry->d_un.d_val;
        break;
      case DT_VERNEED:
        dynamic->versions = entry->d_un.d_ptr;
        break;
      case DT_VERNEEDNUM:
        dynamic->version_count = entry->d_un.d_val;
        break;
    }
  }
  return fail(image, GW_E_IMAGE, "ELF dynamic table has no terminator");
}

/* Marks the image GNU when any DT_NEEDED names libc.so.6 or the GNU loader
   itself. Names are only compared, never resolved or loaded. */
static int scan_needed(struct image *image, const struct dynamic *dynamic,
                       int *glibc) {
  for (size_t j = 0; j < dynamic->count; j++) {
    const Elf64_Dyn *entry = &dynamic->entries[j];
    if (entry->d_tag == DT_NEEDED) {
      char name[MAX_NAMES];
      if (!text_at(image, dynamic, entry->d_un.d_val, name)) {
        return 0;
      }
      if (!strcmp(name, "libc.so.6") || !strcmp(name, GW_LOADER_NAME)) {
        *glibc = 1;
      }
    }
  }

  return 1;
}

/* Walks one Verneed group: GLIBC_* symbol versions mark the image GNU even
   when the parent library name is not libc. The aux chain must end at
   exactly vn_cnt entries. */
static int scan_version_group(struct image *image,
                              const struct dynamic *dynamic, uint64_t address,
                              const Elf64_Verneed *need, int *glibc) {
  if (need->vn_cnt > MAX_VERSIONS) {
    return fail(image, GW_E_CAPACITY,
                "ELF version group exceeds classifier limit");
  }

  char name[MAX_NAMES];
  if (need->vn_version != VER_NEED_CURRENT || !need->vn_cnt ||
      need->vn_aux < sizeof *need || address > UINT64_MAX - need->vn_aux) {
    return fail(image, GW_E_IMAGE, "Invalid ELF version group");
  }
  if (!text_at(image, dynamic, need->vn_file, name)) {
    return 0;
  }

  uint64_t aux = address + need->vn_aux;
  for (unsigned k = 0; k < need->vn_cnt; k++) {
    Elf64_Vernaux version;
    if (!read_va(image, aux, &version, sizeof version) ||
        !text_at(image, dynamic, version.vna_name, name)) {
      return 0;
    }
    if (!strncmp(name, "GLIBC_", 6)) {
      *glibc = 1;
    }
    if (k + 1 == need->vn_cnt) {
      if (version.vna_next) {
        return fail(image, GW_E_IMAGE,
                    "ELF version chain exceeds declared count");
      }
    } else if (version.vna_next < sizeof version ||
               aux > UINT64_MAX - version.vna_next) {
      return fail(image, GW_E_IMAGE, "Invalid ELF version chain");
    }
    aux += version.vna_next;
  }
  return 1;
}

/* Walks the Verneed list declared by DT_VERNEED/DT_VERNEEDNUM. Address and
   count must be both present or both absent, and the chain must end at
   exactly the declared count. */
static int scan_versions(struct image *image, const struct dynamic *dynamic,
                         int *glibc) {
  if (!dynamic->versions && !dynamic->version_count) {
    return 1;
  }

  if (dynamic->version_count > MAX_VERNEEDS) {
    return fail(image, GW_E_CAPACITY,
                "ELF version table exceeds classifier limit");
  }
  if (!dynamic->versions || !dynamic->version_count) {
    return fail(image, GW_E_IMAGE, "ELF version table has no address or count");
  }

  uint64_t address = dynamic->versions;
  for (uint64_t j = 0; j < dynamic->version_count; j++) {
    Elf64_Verneed need;
    if (!read_va(image, address, &need, sizeof need) ||
        !scan_version_group(image, dynamic, address, &need, glibc)) {
      return 0;
    }
    if (j + 1 == dynamic->version_count) {
      if (need.vn_next) {
        return fail(image, GW_E_IMAGE,
                    "ELF version table exceeds declared count");
      }
    } else if (need.vn_next < sizeof need ||
               address > UINT64_MAX - need.vn_next) {
      return fail(image, GW_E_IMAGE, "Invalid ELF version table chain");
    }
    address += need.vn_next;
  }
  return 1;
}

/* Single-entry classifier, metadata only: no mapping, relocation or
   dependency loading. GLIBC -> Guest domain; UNMARKED -> parsed clean with
   no GNU marker, the caller may keep its platform loader; ERROR ->
   unreadable, corrupt or over limit, retrying with the platform loader is
   forbidden. */
enum gw_elf_kind gw_elf_classify(const char *path,
                                 struct gw_loader_error *error) {
  if (error) {
    memset(error, 0, sizeof *error);
  }
  struct image image = {.fd = -1, .error = error};
  if (!path) {
    fail(&image, GW_E_ARGUMENT, "ELF image path required");
    return GW_ELF_ERROR;
  }
  image.fd = open(path, O_RDONLY | O_CLOEXEC);
  if (image.fd < 0) {
    fail(&image, GW_E_IMAGE, "Cannot open ELF image for classification");
    return GW_ELF_ERROR;
  }
  struct dynamic dynamic = {0};
  int glibc = 0;
  int ok = read_headers(&image) && read_dynamic(&image, &dynamic) &&
           scan_needed(&image, &dynamic, &glibc) &&
           scan_versions(&image, &dynamic, &glibc);
  close(image.fd);
  return ok ? (glibc ? GW_ELF_GLIBC : GW_ELF_UNMARKED) : GW_ELF_ERROR;
}
