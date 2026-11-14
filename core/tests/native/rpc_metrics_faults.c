#include "rpc_metrics.h"
#if GW_RPC_PROFILE
#include <errno.h>
#include <time.h>

static int fail_clock;

static int changing_clock(clockid_t clock, struct timespec *value) {
  if (fail_clock) {
    errno = EIO;
    return -1;
  }
  int result = clock_gettime(clock, value);
  errno = EBUSY; /* Even a successful diagnostic clock call must preserve its
                    caller's errno. */
  return result;
}

#define clock_gettime changing_clock
#include "rpc_metrics_internal.h"
#undef clock_gettime

int rpc_metrics_fault_probe(struct gw_rpc_counters *out) {
  struct gw_rpc_row row = {0};
  struct gw_rpc_row *original = gw_rpc_current;
  gw_rpc_current = &row;
  errno = -305;
  fail_clock = 1;

  struct gw_rpc_token failed = gw_rpc_begin();
  gw_rpc_end(failed);
  int preserved = errno == -305;
  fail_clock = 0;

  struct gw_rpc_token success = gw_rpc_begin();
  gw_rpc_end(success);
  preserved = preserved && errno == -305;

  gw_rpc_accumulate(out, &row);
  gw_rpc_current = original;
  return preserved;
}
#endif
