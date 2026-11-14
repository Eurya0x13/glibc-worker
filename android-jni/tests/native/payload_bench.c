#define _GNU_SOURCE
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

#ifndef PAYLOAD_HOST
#define PAYLOAD_HOST 0
#endif

/* One Java caller per fresh process. Configuration/validation are untimed. */
static jobject buffer_ref;
static jbyteArray array_ref;
static unsigned char *shared_bytes, *scratch;
static jint configured_bytes;
static uint64_t calls, copied_elements;
static char *diagnostic_path;

struct payload_stage {
  uint64_t call, start, acquired, released, cpu_start, cpu_acquired,
      cpu_released, tid;
};
static struct payload_stage stages[2400];
static unsigned stage_count;

static uint64_t diagnostic_clock(clockid_t id) {
  struct timespec value;
  return clock_gettime(id, &value)
             ? 0
             : (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static void fail(JNIEnv *env, const char *message) {
  jclass type = (*env)->FindClass(env, "java/lang/AssertionError");

  if (type) {
    (*env)->ThrowNew(env, type, message);
    (*env)->DeleteLocalRef(env, type);
  }
}

static void release(JNIEnv *env) {
  if (buffer_ref) {
    (*env)->DeleteGlobalRef(env, buffer_ref);
  }
  if (array_ref) {
    (*env)->DeleteGlobalRef(env, array_ref);
  }

  buffer_ref = NULL;
  array_ref = NULL;
  shared_bytes = NULL;
  free(scratch);
  scratch = NULL;
}

JNIEXPORT jlong JNICALL Java_benchmark_PayloadModule_configure(
    JNIEnv *env, jclass type, jobject buffer, jbyteArray array, jint bytes) {
  (void)type;
  release(env);

  if (bytes < 0 || (*env)->GetArrayLength(env, array) != bytes ||
      (*env)->GetDirectBufferCapacity(env, buffer) < bytes) {
    fail(env, "Payload configuration size mismatch");
    return 0;
  }

  buffer_ref = (*env)->NewGlobalRef(env, buffer);
  array_ref = (*env)->NewGlobalRef(env, array);
  shared_bytes = (*env)->GetDirectBufferAddress(env, buffer);
  scratch = malloc(bytes ? (size_t)bytes : 1);
  if (!buffer_ref || !array_ref || !shared_bytes || !scratch) {
    fail(env, "Payload allocation/address unavailable");
    return 0;
  }

  /* Prefault both buffers before measuring; this is a reusable-buffer test. */
  memset(scratch, 0x5a, bytes ? (size_t)bytes : 1);
  configured_bytes = bytes;
  calls = copied_elements = 0;
  return ((jlong)(PAYLOAD_HOST ? 1 : 2) << 32) | (uint32_t)syscall(SYS_gettid);
}

JNIEXPORT jlong JNICALL Java_benchmark_PayloadModule_address(JNIEnv *env,
                                                             jclass type) {
  (void)env;
  (void)type;
  return (jlong)(uintptr_t)shared_bytes;
}

JNIEXPORT void JNICALL Java_benchmark_PayloadModule_diagnostics(JNIEnv *env,
                                                                jclass type,
                                                                jstring path) {
  (void)type;
  free(diagnostic_path);
  diagnostic_path = NULL;
  stage_count = 0;

  if (path) {
    const char *text = (*env)->GetStringUTFChars(env, path, NULL);
    if (text) {
      diagnostic_path = strdup(text);
      (*env)->ReleaseStringUTFChars(env, path, text);
    }
    if (!diagnostic_path) {
      fail(env, "Diagnostic path unavailable");
    }
  }
}

JNIEXPORT jint JNICALL
Java_benchmark_PayloadModule_probe(JNIEnv *env, jclass type, jint operation,
                                   jint bytes, jint nonce, jlong address) {
  (void)type;
  if (!scratch || bytes != configured_bytes || operation < 0 || operation > 4 ||
      (uintptr_t)address != (uintptr_t)shared_bytes) {
    fail(env, "Payload probe configuration mismatch");
    return 0;
  }

  calls++;
  unsigned fingerprint = 0;

  if (operation == 1) {
    if ((*env)->GetDirectBufferAddress(env, buffer_ref) != shared_bytes) {
      fail(env, "Direct address changed");
      return 0;
    }
  } else if (operation == 2 || operation == 3) {
    if (operation == 2) {
      memcpy(scratch, shared_bytes, (size_t)bytes);
    } else {
      (*env)->GetByteArrayRegion(env, array_ref, 0, bytes, (jbyte *)scratch);
    }
    if (bytes) {
      fingerprint = scratch[0] ^ scratch[bytes - 1];
    }
  } else if (operation == 4) {
    struct payload_stage *stage =
        diagnostic_path && bytes == 4194304 && stage_count < 2400
            ? &stages[stage_count++]
            : NULL;
    if (stage) {
      stage->call = calls;
      stage->start = diagnostic_clock(CLOCK_MONOTONIC);
      stage->cpu_start = diagnostic_clock(CLOCK_THREAD_CPUTIME_ID);
      stage->tid = syscall(SYS_gettid);
    }
    jboolean copy = JNI_FALSE;
    jbyte *value = (*env)->GetByteArrayElements(env, array_ref, &copy);
    if (stage) {
      stage->acquired = diagnostic_clock(CLOCK_MONOTONIC);
      stage->cpu_acquired = diagnostic_clock(CLOCK_THREAD_CPUTIME_ID);
    }
    if (!value) {
      fail(env, "Array elements unavailable");
      return 0;
    }
    if (bytes) {
      fingerprint = (unsigned char)value[0] ^ (unsigned char)value[bytes - 1];
    }
    copied_elements += copy == JNI_TRUE;
    (*env)->ReleaseByteArrayElements(env, array_ref, value, JNI_ABORT);
    if (stage) {
      stage->released = diagnostic_clock(CLOCK_MONOTONIC);
      stage->cpu_released = diagnostic_clock(CLOCK_THREAD_CPUTIME_ID);
    }
  }
  return nonce ^ bytes ^ (jint)fingerprint;
}

JNIEXPORT jlong JNICALL Java_benchmark_PayloadModule_validate(
    JNIEnv *env, jclass type, jint operation, jint bytes, jint expected_calls) {
  (void)type;
  if (calls != (uint64_t)expected_calls || bytes != configured_bytes ||
      ((operation == 2 || operation == 3) &&
       memcmp(scratch, shared_bytes, (size_t)bytes))) {
    fail(env, "Payload full-buffer validation/call count failed");
    return -1;
  }

  return (jlong)copied_elements;
}

JNIEXPORT void JNICALL Java_benchmark_PayloadModule_finish(JNIEnv *env,
                                                           jclass type) {
  (void)type;
  release(env);

  if (diagnostic_path) {
    FILE *file = fopen(diagnostic_path, "wb");
    uint64_t header[] = {0x47575331, 1, sizeof(struct payload_stage),
                         stage_count};
    int ok = file && fwrite(header, sizeof header, 1, file) == 1 &&
             fwrite(stages, sizeof stages[0], stage_count, file) == stage_count;
    if (file && fclose(file)) {
      ok = 0;
    }
    free(diagnostic_path);
    diagnostic_path = NULL;
    if (!ok) {
      fail(env, "Cannot save diagnostic stages");
    }
  }
}
