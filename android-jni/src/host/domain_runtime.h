#pragma once
/* Private same-thread transport shared by the installed runtime and fixtures.
 */
#include "domain_transport.h"
#include "host_internal.h"
#include "loader_image.h"
#include "signals.h"
#include "signal-router.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define DOMAIN_INTERNAL __attribute__((visibility("hidden")))
#ifdef GW_DOMAIN_NONLOCAL
#include "nonlocal.h"

struct gw_domain_rpc_scope {
  struct gw_nonlocal_scope unwind;
  struct gw_domain_rpc_scope *volatile previous;
  void *volatile guest_checkpoint;
};

DOMAIN_INTERNAL void gw_domain_rpc_enter(struct gw_domain_rpc_scope *);
DOMAIN_INTERNAL int gw_domain_rpc_leave(struct gw_domain_rpc_scope *, uint64_t);
DOMAIN_INTERNAL int gw_domain_rpc_recoverable(void);
DOMAIN_INTERNAL void gw_domain_signal_notification(int, siginfo_t *, void *);
#endif
extern DOMAIN_INTERNAL struct jni_domain_session session;
extern DOMAIN_INTERNAL _Atomic int domains_created, domains_finished;
extern DOMAIN_INTERNAL _Atomic int fail_next_context, expected_errors;
DOMAIN_INTERNAL int gw_domain_host_active(void);
DOMAIN_INTERNAL int owner_command(struct jni_domain_context *, int operation);
DOMAIN_INTERNAL int domain_owner_start(const char *, const char *,
                                       const char *);
DOMAIN_INTERNAL void start_owner(char **arguments);

#define REQUIRE(x)                                                        \
  do {                                                                    \
    if (!(x)) {                                                           \
      fprintf(stderr, "JNI_DOMAIN_FAILURE source=%s line=%d\n", __FILE__, \
              __LINE__);                                                  \
      fflush(stdout);                                                     \
      _exit(126);                                                         \
    }                                                                     \
  } while (0)
