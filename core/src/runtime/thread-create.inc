/* Optional GNU thread creation admission, included by pthread_create.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Native descriptor ownership can move to an exiting detached child as soon
 * as clone succeeds. Only the opaque cookie survives that transfer:
 * https://github.com/bminor/glibc/blob/glibc-2.44/nptl/pthread_create.c
 */
#include <gw-thread-create.h>

static struct gw_libc_thread_create_hook gw_create_hook;
static unsigned gw_create_hook_state;

int __gw_domain_thread_create_hook(
    unsigned version, const struct gw_libc_thread_create_hook *hook) {
  if (version != 1 || !hook || hook->size != sizeof *hook || !hook->prepare ||
      !hook->complete || !hook->enter || !hook->leave) {
    return EINVAL;
  }

  unsigned expected = 0;
  if (!__atomic_compare_exchange_n(&gw_create_hook_state, &expected, 1, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return EBUSY;
  }
  gw_create_hook = *hook;
  __atomic_store_n(&gw_create_hook_state, 2, __ATOMIC_RELEASE);
  return 0;
}

static void gw_create_event(struct pthread *pd, int entering) {
  if (!pd->gw_create_prepared) {
    return;
  }

  int saved = errno;
  internal_sigset_t previous;
  internal_signal_block_all(&previous);
  if (entering) {
    gw_create_hook.enter(pd->gw_create_cookie);
  } else {
    gw_create_hook.leave(pd->gw_create_cookie);
    pd->gw_create_prepared = false;
    pd->gw_create_cookie = NULL;
  }
  __set_errno(saved);
  internal_signal_restore_set(&previous);
}

static int create_thread(struct pthread *pd, const struct pthread_attr *attr,
                         bool *stopped_start, void *stackaddr, size_t stacksize,
                         bool *thread_ran) {
  const struct gw_libc_thread_create_hook *hook =
      __atomic_load_n(&gw_create_hook_state, __ATOMIC_ACQUIRE) == 2
          ? &gw_create_hook
          : NULL;
  void *cookie = NULL;
  pd->gw_create_prepared = false;
  pd->gw_create_cookie = NULL;
  if (hook) {
    int saved = errno;
    int error = hook->prepare(pd, pd->gw_inherited_mask,
                              pd->gw_inherited_carrier, &cookie);
    __set_errno(saved);
    if (error) {
      return error;
    }
    pd->gw_create_cookie = cookie;
    pd->gw_create_prepared = true;
  }

  /* Capture clone's positive return in parent-local storage. Reading pd->tid
   * after this call would race detached child exit, unmap and descriptor reuse.
   * Completion waits for native affinity/scheduling setup's final status. */
  int tid = 0;
  int error = gw_create_native(pd, attr, stopped_start, stackaddr, stacksize,
                               thread_ran, &tid);
  if (hook) {
    int saved = errno;
    hook->complete(cookie, tid, error);
    __set_errno(saved);
  }
  return error;
}
