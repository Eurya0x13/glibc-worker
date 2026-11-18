#ifndef GW_JOB_CONTROL_H
#define GW_JOB_CONTROL_H
#include <signal.h>
#include <stdint.h>

#define GW_JOB_CONTROL_MASK                                          \
  ((UINT64_C(1) << (SIGTSTP - 1)) | (UINT64_C(1) << (SIGTTIN - 1)) | \
   (UINT64_C(1) << (SIGTTOU - 1)) | (UINT64_C(1) << (SIGCONT - 1)))
#endif
