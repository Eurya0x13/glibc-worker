#ifndef GW_JRE17_ADAPTER_H
#define GW_JRE17_ADAPTER_H
#include "jvm_adapter.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Compatibility names only: both functions delegate to the selected generic
 * JVM profile. No JRE17 implementation lives here. New clients include
 * jvm_adapter.h and use gw_jvm_adapter_install / gw_jvm_adapter_restore. */
int gw_jre17_adapter_install(JNIEnv *, const char *native_dir,
                             const char *libjava_path,
                             struct gw_loader_error *);
int gw_jre17_adapter_restore(JNIEnv *, struct gw_loader_error *);
#ifdef __cplusplus
}
#endif
#endif
