#ifndef JNI_PROXY_H
#define JNI_PROXY_H
#include <jni.h>
#include "protocol.h"
/* Guest-only virtual context; never attaches a glibc thread to ART. */
JavaVM *guest_jni_context_begin(struct shared *shared);
void guest_jni_context_end(void);
#endif
