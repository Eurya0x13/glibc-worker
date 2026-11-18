#define _GNU_SOURCE
#include "guest-fd-lease.h"
#include "guest-epoll.h"
#include "guest-signalfd.h"
#include "nonlocal-mask.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>

static __thread struct gw_guest_fd_lease *loans;

int gw_guest_fd_lease_open(struct gw_guest_fd_lease **out,
                           struct gw_signal_target *target, int fd,
                           uintptr_t stack) {
  struct gw_guest_fd_lease *loan =
      mmap(NULL, sizeof *loan, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (loan == MAP_FAILED) {
    return -errno;
  }

  int copy = fd < 0 ? -1 : syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 0);
  if (fd >= 0 && copy < 0) {
    int error = errno;
    munmap(loan, sizeof *loan);
    return -error;
  }

  *loan = (struct gw_guest_fd_lease){.target = target,
                                     .next = loans,
                                     .scope = gw_guest_fd_scope(),
                                     .stack = stack,
                                     .fd = copy};
  loans = loan;
  *out = loan;
  return fd < 0 ? 0 : copy;
}

void *gw_guest_fd_lease_reserve(struct gw_guest_fd_lease *loan, size_t bytes) {
  if (bytes <= loan->size) {
    return loan->storage;
  }

  void *buffer = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (buffer == MAP_FAILED) {
    return NULL;
  }
  if (loan->storage) {
    memcpy(buffer, loan->storage, loan->size);
    munmap(loan->storage, loan->size);
  }
  loan->storage = buffer;
  loan->size = bytes;
  return buffer;
}

static void release(struct gw_guest_fd_lease **link) {
  struct gw_guest_fd_lease *loan = *link;
  *link = loan->next;

  if (loan->fd >= 0) {
    gw_guest_fds_lock();
    syscall(SYS_close, loan->fd);
    int retired = gw_guest_signalfd_reap();
    retired |= gw_guest_epoll_reap();
    if (retired) {
      gw_signal_targets_wake_waiters(loan->target->registry);
    }
    gw_guest_fds_unlock();
  }
  if (loan->storage) {
    munmap(loan->storage, loan->size);
  }
  munmap(loan, sizeof *loan);
}

void gw_guest_fd_lease_drop(struct gw_guest_fd_lease **out) {
  if (!*out) {
    return;
  }
  int saved = errno;
  uint64_t physical = gw_nonlocal_mask_block();
  struct gw_guest_fd_lease **link = &loans;
  while (*link && *link != *out) {
    link = &(*link)->next;
  }
  if (*link) {
    release(link);
  }
  *out = NULL;
  errno = saved;
  gw_nonlocal_mask_restore(physical);
}

void gw_guest_fd_lease_unwind(uintptr_t destination, uintptr_t stack_top) {
  if (!loans) {
    return;
  }
  /* Match glibc's cleanup comparison, including a pthread stack allocated
   * below its alternate signal stack. The bridge scope test protects older
   * suspended native activations from this native jump.
   * https://github.com/bminor/glibc/blob/master/nptl/pthread_cleanup_upto.c
   */
  int saved = errno;
  uint64_t physical = gw_nonlocal_mask_block();
  const void *scope = gw_guest_fd_scope();
  for (struct gw_guest_fd_lease **link = &loans; *link;) {
    struct gw_guest_fd_lease *loan = *link;
    if (loan->scope == scope &&
        loan->stack - stack_top < destination - stack_top) {
      release(link);
    } else {
      link = &loan->next;
    }
  }
  errno = saved;
  gw_nonlocal_mask_restore(physical);
}

void gw_guest_fd_lease_context(uintptr_t destination, uintptr_t low,
                               size_t size) {
  if (!loans) {
    return;
  }
  int saved = errno;
  uint64_t physical = gw_nonlocal_mask_block();
  const void *scope = gw_guest_fd_scope();
  for (struct gw_guest_fd_lease **link = &loans; *link;) {
    struct gw_guest_fd_lease *loan = *link;
    if (loan->scope == scope && loan->stack - low < size &&
        loan->stack < destination) {
      release(link);
    } else {
      link = &loan->next;
    }
  }
  errno = saved;
  gw_nonlocal_mask_restore(physical);
}

void gw_guest_fd_lease_end(const void *scope) {
  if (!loans) {
    return;
  }
  int saved = errno;
  uint64_t physical = gw_nonlocal_mask_block();
  for (struct gw_guest_fd_lease **link = &loans; *link;) {
    if ((*link)->scope == scope) {
      release(link);
    } else {
      link = &(*link)->next;
    }
  }
  errno = saved;
  gw_nonlocal_mask_restore(physical);
}

void gw_guest_fd_lease_finish(struct gw_signal_target *target) {
  int saved = errno;
  uint64_t physical = gw_nonlocal_mask_block();
  for (struct gw_guest_fd_lease **link = &loans; *link;) {
    if ((*link)->target == target) {
      release(link);
    } else {
      link = &(*link)->next;
    }
  }
  errno = saved;
  gw_nonlocal_mask_restore(physical);
}
