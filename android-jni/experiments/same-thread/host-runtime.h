#pragma once
/* Internal experiment API shared by the disposable harness and Pojav adapter.
 * It does not alter the installed bridge ABI or production defaults. */
#include "transport.h"
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
extern DOMAIN_INTERNAL struct jni_domain_session session;
extern DOMAIN_INTERNAL _Atomic int domains_created, domains_finished;
extern DOMAIN_INTERNAL _Atomic int fail_next_context, expected_errors;
DOMAIN_INTERNAL int owner_command(struct jni_domain_context *, int operation);
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
