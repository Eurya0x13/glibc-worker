#ifndef GW_EXPERIMENT_HOST_HOOKS_H
#define GW_EXPERIMENT_HOST_HOOKS_H
#include "domain.h"

/* Optional fixture extension. Called only with the Host stack and TLS. */
int experiment_start(void);
int experiment_thread_enter(void);
int experiment_callback(struct exchange *, int, int (*)(struct exchange *));
int experiment_thread_leave(void);
int experiment_finish(void);
#endif
