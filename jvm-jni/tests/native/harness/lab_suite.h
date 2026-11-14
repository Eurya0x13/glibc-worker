#ifndef GW_LAB_SUITE_H
#define GW_LAB_SUITE_H
#include "full_protocol.h"
struct shared;
int full_host_suite(struct shared *);
int gw_lab_suite(struct full_bridge *, JNIEnv *);
int gw_lab_prepare(struct full_bridge *, JNIEnv *, const char *native_dir);
#endif
