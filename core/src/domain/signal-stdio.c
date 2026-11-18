#define _GNU_SOURCE
#include "signal-stdio.h"
#include "signal-fd-adapter.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static _Atomic int active;
_Static_assert(sizeof(void *) == 8, "reviewed Host stdio profiles are LP64");

#if defined(__GLIBC__)
#include <link.h>
#include <sys/mman.h>

enum { READ_SLOT = 14, TABLE_WORDS = 21 };

struct native_table {
  void **slot;
  void *original;
  uintptr_t page;
};

static struct native_table tables[2];
static size_t page_size;
static ssize_t (*native_read)(FILE *, void *, ssize_t);
static _Atomic int quarantined;

static ssize_t invoke_native(void *stream, void *buffer, size_t size) {
  return native_read(stream, buffer, (ssize_t)size);
}

static ssize_t translated_read(FILE *stream, void *buffer, ssize_t size) {
  /* This GNU profile already verifies the owning FILE read tables. Its
   * _IO_FLAGS2_NOTCANCEL is part of the reviewed GNU libio layout.
   * https://github.com/bminor/glibc/blob/glibc-2.42/libio/libio.h */
  enum gw_libc_io_policy policy =
      stream->_flags2 & 2 ? GW_IO_NOCANCEL : GW_IO_CANCELLABLE;
  return gw_signal_fd_read_policy(fileno(stream), buffer, (size_t)size,
                                  invoke_native, stream, policy);
}

struct readonly_range {
  uintptr_t address;
  int found;
};

static int inspect_relro(struct dl_phdr_info *image, size_t size, void *data) {
  (void)size;
  struct readonly_range *range = data;

  for (unsigned i = 0; i < image->dlpi_phnum; ++i) {
    const ElfW(Phdr) *header = &image->dlpi_phdr[i];
    uintptr_t begin = image->dlpi_addr + header->p_vaddr;
    uintptr_t first = begin & ~(uintptr_t)(page_size - 1);
    uintptr_t last = (begin + header->p_memsz) & ~(uintptr_t)(page_size - 1);
    uintptr_t page = range->address & ~(uintptr_t)(page_size - 1);
    /* Match the loader's complete protected pages, not the last partial
     * RELRO page, which can also contain live writable libc state.
     * https://github.com/bminor/glibc/blob/glibc-2.41/elf/dl-reloc.c */
    if (header->p_type == PT_GNU_RELRO && page >= first &&
        page + page_size <= last) {
      range->found = 1;
      return 1;
    }
  }

  return 0;
}

static int inspect_table(const char *name, struct native_table *table) {
  void *address = dlsym(RTLD_NEXT, name);
  Dl_info location, function_location;
  const ElfW(Sym) * symbol;
  if (!address ||
      !dladdr1(address, &location, (void **)&symbol, RTLD_DL_SYMENT) ||
      !symbol || symbol->st_size != TABLE_WORDS * sizeof(void *) ||
      !dladdr((void *)native_read, &function_location) ||
      location.dli_fbase != function_location.dli_fbase) {
    return ENOTSUP;
  }

  table->slot = (void **)address + READ_SLOT;
  table->original = __atomic_load_n(table->slot, __ATOMIC_ACQUIRE);
  if (table->original != (void *)native_read) {
    return ENOTSUP;
  }

  struct readonly_range range = {.address = (uintptr_t)table->slot};
  dl_iterate_phdr(inspect_relro, &range);
  if (!range.found) {
    return ENOTSUP;
  }

  table->page = range.address & ~(uintptr_t)(page_size - 1);
  return 0;
}

static int replace_slot(struct native_table *table, void *expected,
                        void *replacement) {
  if (mprotect((void *)table->page, page_size, PROT_READ | PROT_WRITE)) {
    return errno;
  }

  int replaced =
      __atomic_compare_exchange_n(table->slot, &expected, replacement, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);

  if (mprotect((void *)table->page, page_size, PROT_READ)) {
    int error = errno;
    if (replaced) {
      __atomic_store_n(table->slot, expected, __ATOMIC_RELEASE);
    }

    /* A failed protection rollback is terminal. Keep all code and callbacks
     * pinned and report EIO: the embedder must discard the process. */
    if (mprotect((void *)table->page, page_size, PROT_READ)) {
      atomic_store_explicit(&quarantined, 1, memory_order_release);
      return EIO;
    }
    return error;
  }

  return replaced ? 0 : EBUSY;
}

int signal_stdio_enable(void) {
  if (atomic_load_explicit(&quarantined, memory_order_acquire)) {
    return EIO;
  }
  if (atomic_load_explicit(&active, memory_order_acquire)) {
    return EALREADY;
  }

  page_size = (size_t)sysconf(_SC_PAGESIZE);
  native_read = (void *)dlsym(RTLD_NEXT, "_IO_file_read");
  if (!native_read || !page_size || (page_size & (page_size - 1))) {
    return ENOTSUP;
  }

  int error = inspect_table("_IO_file_jumps", &tables[0]);
  if (!error) {
    error = inspect_table("_IO_wfile_jumps", &tables[1]);
  }
  if (error) {
    return error;
  }

  /* Keep native FILE/vtable identity and the libc's vtable validation. Only
   * its existing read callbacks change, during explicit bootstrap quiescence.
   * Wide and byte reads both retain native buffering, locks and seek/close.
   * https://github.com/bminor/glibc/blob/glibc-2.41/libio/libioP.h
   * https://github.com/bminor/glibc/blob/glibc-2.41/libio/fileops.c
   * https://github.com/bminor/glibc/blob/glibc-2.41/libio/wfileops.c */
  error = replace_slot(&tables[0], tables[0].original, (void *)translated_read);
  if (!error) {
    error =
        replace_slot(&tables[1], tables[1].original, (void *)translated_read);
  }

  if (error) {
    for (unsigned i = 0; i < 2; ++i) {
      if (__atomic_load_n(tables[i].slot, __ATOMIC_ACQUIRE) ==
          (void *)translated_read) {
        if (replace_slot(&tables[i], (void *)translated_read,
                         tables[i].original)) {
          atomic_store_explicit(&quarantined, 1, memory_order_release);
          return EIO;
        }
      }
    }
    return error;
  }

  atomic_store_explicit(&active, 1, memory_order_release);
  return 0;
}

int signal_stdio_verify(void) {
  if (atomic_load_explicit(&quarantined, memory_order_acquire)) {
    return EIO;
  }
  if (!atomic_load_explicit(&active, memory_order_acquire)) {
    return 0;
  }

  for (unsigned i = 0; i < 2; ++i) {
    if (__atomic_load_n(tables[i].slot, __ATOMIC_ACQUIRE) !=
        (void *)translated_read) {
      return EBUSY;
    }
  }

  return 0;
}

int signal_stdio_disable(void) {
  int error = signal_stdio_verify();
  if (error || !atomic_load_explicit(&active, memory_order_acquire)) {
    return error;
  }

  for (unsigned i = 0; i < 2; ++i) {
    error =
        replace_slot(&tables[i], (void *)translated_read, tables[i].original);
    if (error) {
      for (unsigned j = 0; j < i; ++j) {
        if (replace_slot(&tables[j], tables[j].original,
                         (void *)translated_read)) {
          atomic_store_explicit(&quarantined, 1, memory_order_release);
          return EIO;
        }
      }
      return error;
    }
  }

  atomic_store_explicit(&active, 0, memory_order_release);
  return 0;
}

const char *signal_stdio_profile(void) {
  return atomic_load_explicit(&active, memory_order_acquire)
             ? "glibc-native-read-tables-lp64-v1"
             : NULL;
}

#elif defined(__ANDROID__)

/* Bionic's native stdio already calls the interposed public read backend.
 * Verify the running artifact's actual path before enabling this channel;
 * no private FILE layout, replacement cookie or libc data patch is needed. */
extern int gw_signal_fd_probe_stdio(void);

int signal_stdio_enable(void) {
  if (atomic_load_explicit(&active, memory_order_acquire)) {
    return EALREADY;
  }

  int error = gw_signal_fd_probe_stdio();
  if (!error) {
    atomic_store_explicit(&active, 1, memory_order_release);
  }

  return error;
}

int signal_stdio_verify(void) {
  return 0;
}

int signal_stdio_disable(void) {
  atomic_store_explicit(&active, 0, memory_order_release);
  return 0;
}

const char *signal_stdio_profile(void) {
  return atomic_load_explicit(&active, memory_order_acquire)
             ? "bionic-verified-public-read"
             : NULL;
}

#else
#error Unsupported Host stdio profile
#endif
