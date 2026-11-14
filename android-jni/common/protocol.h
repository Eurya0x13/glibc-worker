#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <stdatomic.h>
#include <stdint.h>
#define MAGIC UINT64_C(0x475545535453504b)

/* Smoke-protocol probe payload: plain values only, no pointers into either
   libc's private state. */
struct packet {
  int32_t value, callback_value;
  uint64_t guest_tp;
};

typedef int (*callback_t)(struct packet *);
typedef int (*probe_t)(struct packet *, callback_t);

/* A single Guest owner serializes JNI requests. Handles are Host table tokens,
   never ART reference addresses. This protocol supports only the smoke subset.
 */
enum jni_op {
  J_VERSION = 1,
  J_FIND_CLASS,
  J_STATIC_METHOD,
  J_CALL_INT,
  J_EXCEPTION_CHECK,
  J_EXCEPTION_CLEAR,
  J_DELETE_LOCAL
};

/* One outstanding smoke-subset JNI request; fixed-size POD both sides must
   agree on. The sequence words gate re-execution exactly as fb_call does. */
struct jni_rpc {
  _Atomic int request, response;
  int op, arg, error;
  uintptr_t object, method;
  int64_t result;
  char name[128], signature[32];
};

/* No libc-owned objects cross this boundary. All atomics must be lock-free. */
struct shared {
  uint64_t magic;
  int host_pid, host_tid;
  _Atomic int phase; /* 0=start, 1=guest main, 2=tests done, -1=failed */
  _Atomic int request, response, stop;
  uintptr_t packet;
  int reply;
  int guest_pid, guest_tid, probe_rc, pthread_rc, pthread_value;
  uintptr_t guest_tp, child_tp, guest_errno_ptr;
  int guest_errno_after, guest_tls_after, guest_libc_found;
  int errno_after_create, errno_after_join;
  _Atomic int child_ready, child_release;
  int thread_isolation_ok;
  int access_rc;
  uintptr_t full_bridge;
  struct jni_rpc jni;
  int jni_onload_version, jni_status, jni_bridge_error, jni_requests;
  _Atomic unsigned heartbeat;
};
#endif
