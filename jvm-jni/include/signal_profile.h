#ifndef GW_JVM_SIGNAL_PROFILE_H
#define GW_JVM_SIGNAL_PROFILE_H
#include <jni.h>

/* Explicit, pinned HotSpot/ART artifact integration. Call after VM startup,
 * before releasing application code or creating Guest carriers. The DSO stays
 * loaded until process disposal. No process-wide ELF interposition is claimed.
 * Build with a maintainer-reviewed GW_SIGNAL_PROFILE_HEADER. Unconfigured
 * builds return ENOTSUP; neither VM family nor offsets are auto-detected. */
int domain_late_signal_install(JNIEnv *env);
int domain_late_signal_restore(void);

/* After installing a reviewed profile and before starting Guest helpers,
 * reserve a process-owned RT number for native recipient acknowledgements.
 * Reclaim from a Host control thread under the signal_channel.h contract. */
int domain_late_signal_reclaim_enable(int number);
int domain_late_signal_reclaim(unsigned timeout_ms, unsigned *reclaimed);
#endif
