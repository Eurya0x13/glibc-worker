/* Private libc interface. Never installed as an application ABI.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GW_LIBC_WAIT_MASK_H
#define GW_LIBC_WAIT_MASK_H
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <signal-consume-hook.h>

struct gw_domain_wait_mask {
  sigset_t kernel;
  uint64_t physical, logical;
  int active;
};

int __gw_domain_wait_begin(const sigset_t *, struct gw_domain_wait_mask *);
int __gw_domain_wait_end(struct gw_domain_wait_mask *);
int __gw_domain_wait_filter(const sigset_t *, sigset_t *);
int __gw_domain_consume_pending(sigset_t *, int *);
int __gw_domain_consume_wait(const sigset_t *, siginfo_t *,
                             const struct timespec *, gw_libc_wait_event,
                             int *);
libc_hidden_proto(__gw_domain_wait_begin);
libc_hidden_proto(__gw_domain_wait_end);
libc_hidden_proto(__gw_domain_wait_filter);
libc_hidden_proto(__gw_domain_consume_pending);
libc_hidden_proto(__gw_domain_consume_wait);

/* Keep the owning libc's SYSCALL_CANCEL, timeout conversion and error paths.
 * Only the mask transaction changes. NULL masks retain native semantics. */
#define GW_DOMAIN_WAIT_BEGIN(set)                         \
  struct gw_domain_wait_mask gw_wait = {0};               \
  do {                                                    \
    int gw_error = __gw_domain_wait_begin(set, &gw_wait); \
    if (gw_error) {                                       \
      __set_errno(gw_error);                              \
      return -1;                                          \
    }                                                     \
    if (gw_wait.active) {                                 \
      set = &gw_wait.kernel;                              \
    }                                                     \
  } while (0)

#define GW_DOMAIN_WAIT_RETURN(call)                \
  do {                                             \
    int gw_result = (call);                        \
    int gw_errno = errno;                          \
    int gw_error = __gw_domain_wait_end(&gw_wait); \
    __set_errno(gw_error ? gw_error : gw_errno);   \
    return gw_error ? -1 : gw_result;              \
  } while (0)
#endif
