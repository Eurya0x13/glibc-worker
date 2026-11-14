#ifndef GW_LOADER_SMOKE_H
#define GW_LOADER_SMOKE_H
#include <jni.h>
int gw_loader_smoke_not_ready(JNIEnv *, const char *native_dir);
int gw_loader_smoke(JNIEnv *, const char *native_dir);
#endif
