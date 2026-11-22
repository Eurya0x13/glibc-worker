#ifndef GW_JNI_BYTES_H
#define GW_JNI_BYTES_H

#include <jni.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Transfer native bytes without the modified UTF-8 used by JNI strings. */
static inline jbyteArray gw_jni_new_bytes(JNIEnv *env, const char *text) {
  size_t length = strlen(text);
  if (length > INT32_MAX) {
    return NULL;
  }

  jbyteArray bytes = (*env)->NewByteArray(env, (jsize)length);
  if (!bytes) {
    return NULL;
  }

  (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)length, (const jbyte *)text);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->DeleteLocalRef(env, bytes);
    return NULL;
  }
  return bytes;
}

static inline bool gw_jni_read_path(JNIEnv *env, jbyteArray bytes, char *path,
                                    size_t capacity) {
  jsize length = (*env)->GetArrayLength(env, bytes);
  if (length <= 0 || (size_t)length >= capacity) {
    return false;
  }

  (*env)->GetByteArrayRegion(env, bytes, 0, length, (jbyte *)path);
  if ((*env)->ExceptionCheck(env) || memchr(path, '\0', (size_t)length)) {
    return false;
  }
  path[length] = '\0';
  return true;
}

#endif
