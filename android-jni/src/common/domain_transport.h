#pragma once
/* Private, versioned transport shared by the matching Host and GNU service.
 * Public callers configure the runtime through runtime_bridge.h. */
#include "domain.h"
#include "protocol.h"
#include "full_bridge.h"
#include <stdatomic.h>
#include <string.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>

/* Selection for adoption-enabled entrypoints only. Normal installed SDK builds
 * have their own lifecycle and do not consume this experiment's environment.
 * Missing selection means same-thread; worker is an explicit compatibility
 * choice. Reject typos instead of quietly changing the execution contract. */
static inline int jni_domain_select(const char *mode) {
  if (!mode || !strcmp(mode, "domain")) {
    return 1;
  }
  if (!strcmp(mode, "worker")) {
    return 0;
  }
  return -1;
}

#define GW_DOMAIN_TRANSPORT_MAGIC UINT64_C(0x4757444f4d41494e)
#define GW_DOMAIN_TRANSPORT_VERSION 13u
#define GW_DOMAIN_LIBC_CAPABILITIES UINT64_C(0x475744010000000a)
#define GW_DOMAIN_CAP_ADOPTION UINT64_C(1)
#define GW_DOMAIN_CAP_NATIVE_UNWIND UINT64_C(2)
#define GW_DOMAIN_CAP_STOP_QUIESCENT UINT64_C(4)
#define GW_DOMAIN_OWNER_NAME "gw-guest-owner"
#define GW_DOMAIN_STOP_MASK \
  (UINT64_MAX &             \
   ~((UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1))))

struct jni_domain_handshake {
  uint64_t magic;
  uint32_t version, size;
  uint32_t context_size, exchange_size;
  uint32_t siginfo_size, ucontext_size, sigmask_offset, mcontext_offset;
  uint64_t required, accepted;
};

enum { JNI_DOMAIN_ALLOC = 1, JNI_DOMAIN_FREE, JNI_DOMAIN_STOP };

struct jni_domain_context {
  struct exchange exchange;
  void *token;
  _Atomic int stop, ready;
  int id;
  int domain;
  int fail_allocate;
  void *abort_checkpoint;
  struct fb_call *abort_call;
  void (*abort_entry)(void *);
  uintptr_t abort_stack;
  uint64_t guest_mask;
  int guest_native_active;
  int executing_guest;
  stack_t guest_altstack;
  /* Opaque, owning-domain addresses. Only the owner dereferences them. */
  void *host_rpc;
  void *jump_record;
  void *jump_target;
  int jump_value, jump_resume, jump_kind;
  void *guest_rpc_owner;
  int abort_resume;
};

struct jni_domain_session {
  struct shared shared;
  struct jni_domain_handshake handshake;
  struct jni_domain_context *context;
  _Atomic int request, response, ready;
  int operation, error;
  int domain;
  uint64_t reserved_signals;
  void (*entry)(void *);
  void (*note_guest)(int, long, uintptr_t);
  void (*guest_thread)(int);
  int (*bind_guest_signal)(int (*)(int, void *, void *));
  /* Published with STOP response, after the native kernel mask is installed.
   * The supervisor never resumes application work or unmasks after this ACK. */
  uint64_t owner_stopped_mask;
};

static inline void wake_word(_Atomic int *p) {
  syscall(SYS_futex, p, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static inline void wait_word(_Atomic int *p, int v) {
  syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
}
