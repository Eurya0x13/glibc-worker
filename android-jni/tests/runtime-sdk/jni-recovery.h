#ifndef GW_SDK_JNI_RECOVERY_H
#define GW_SDK_JNI_RECOVERY_H
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>

enum sdk_jni_resource {
  SDK_LOCAL_REF,
  SDK_GLOBAL_REF,
  SDK_WEAK_REF,
  SDK_STRING_CHARS,
  SDK_UTF_CHARS,
  SDK_BOOLEAN_ELEMENTS,
  SDK_BYTE_ELEMENTS,
  SDK_CHAR_ELEMENTS,
  SDK_SHORT_ELEMENTS,
  SDK_INT_ELEMENTS,
  SDK_LONG_ELEMENTS,
  SDK_FLOAT_ELEMENTS,
  SDK_DOUBLE_ELEMENTS,
  SDK_STRING_CRITICAL,
  SDK_ARRAY_CRITICAL,
  SDK_LOCAL_FRAME,
  SDK_MONITOR,
  SDK_METHOD_ID,
  SDK_FIELD_ID
};

enum sdk_jni_transfer {
  SDK_JNI_NORMAL,
  SDK_JNI_TO_HOST,
  SDK_JNI_TO_GUEST_JUMP,
  SDK_JNI_TO_GUEST_CONTEXT,
  SDK_JNI_NESTED,
  SDK_JNI_JUMP_SOURCE,
  SDK_JNI_CONTEXT_SOURCE,
  SDK_JNI_GUARD,
  SDK_JNI_ASYNC_JUMP,
  SDK_JNI_ASYNC_CONTEXT
};

enum { SDK_COPY_SENTINEL = 0x6f };

struct sdk_jni_witness {
  int armed;
  jboolean copy;
  uintptr_t retired_token;
  int stale_token_rejected;
  _Atomic int signal_requested, signal_delivered;
};

jint sdk_try_guarded_context(JNIEnv *, jclass);
struct gw_loader_api_v1;
void sdk_run_jni_recovery(JNIEnv *, jclass, const struct gw_loader_api_v1 *,
                          uint64_t);
void sdk_run_jni_recovery_async(JNIEnv *, jclass,
                                const struct gw_loader_api_v1 *, uint64_t);
#endif
