/* Optional pre-clone admission for the owning runtime.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GW_LIBC_THREAD_CREATE_H
#define GW_LIBC_THREAD_CREATE_H
#include <stddef.h>
#include <stdint.h>

struct gw_libc_thread_create_hook {
  size_t size;
  /* All catchable signals are blocked. Positive error means no clone and no
   * completion; the callback must clean its own partial preparation. */
  int (*prepare)(void *key, uint64_t logical, uint64_t physical, void **cookie);
  /* Exactly once after a successful prepare. tid is the raw clone result,
   * including when later scheduling/affinity setup fails. key/TCB may already
   * have exited or been reused: use only the separately owned cookie. */
  void (*complete)(void *cookie, int tid, int error);
  /* The actual child, before initial unmasking and after TLS destructors.
   * These may precede parent completion. Failed native setup skips both.
   * No application code or nonlocal exits are permitted in these callbacks. */
  void (*enter)(void *cookie);
  void (*leave)(void *cookie);
};

int __gw_domain_thread_create_hook(unsigned,
                                   const struct gw_libc_thread_create_hook *);
#endif
