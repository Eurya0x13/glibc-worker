#ifndef GW_EXPERIMENT_LATE_SIGNAL_H
#define GW_EXPERIMENT_LATE_SIGNAL_H
#include <jni.h>

/* Experimental, pinned HotSpot artifact only. Call after VM startup, before
 * releasing application code or creating Guest carriers. The DSO stays loaded
 * until process disposal. No process-wide ELF interposition is claimed. */
int domain_late_signal_install(JNIEnv *env);
int domain_late_signal_restore(void);
#endif
