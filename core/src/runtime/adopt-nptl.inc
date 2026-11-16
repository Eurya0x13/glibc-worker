/* glibc 2.44 NPTL adoption extension, included by pthread_create.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Calls originate inside this Guest libc, never with Host TLS installed.
 * The supervisor owns the supplied stack until release has returned. */

int __gw_domain_alloc(void *, size_t, void **, void **, void **);
int __gw_domain_bind(void *);
int __gw_domain_run(void (*)(void *), void *);
int __gw_domain_finish(void);
int __gw_domain_free(void *);

int __gw_domain_alloc(void *memory, size_t size, void **token,
                      void **thread_pointer, void **stack_top) {
  /* Allocation is serialized by the Guest owner.  Its existing thread is
     alive throughout every adopted domain's lifetime. */
  if (memory == NULL || size < 65536 || token == NULL ||
      thread_pointer == NULL || stack_top == NULL) {
    return EINVAL;
  }
  if (__libc_single_threaded_internal) {
    late_init();
    __libc_single_threaded_internal = 0;
    __libc_single_threaded = 0;
    _IO_enable_locks();
    THREAD_SETMEM(THREAD_SELF, header.multiple_threads, 1);
  }
  struct pthread_attr attr = {0};
  attr.flags = ATTR_FLAG_STACKADDR | ATTR_FLAG_DETACHSTATE;
  attr.stackaddr = (char *)memory + size;
  attr.stacksize = size;
  struct pthread *pd = NULL;
  void *stack = NULL;
  size_t usable = 0;
  int error = allocate_stack(&attr, &pd, &stack, &usable);
  if (error != 0) {
    return error;
  }
#if TLS_TCB_AT_TP
  pd->header.self = pd;
  pd->header.tcb = pd;
#endif
  pd->flags = ATTR_FLAG_GW_DOMAIN | ATTR_FLAG_DETACHSTATE;
  pd->joinstate = THREAD_STATE_DETACHED;
  pd->cancelhandling = CANCELSTATE_BITMASK;
  pd->schedpolicy = THREAD_SELF->schedpolicy;
  pd->schedparam = THREAD_SELF->schedparam;
#ifdef THREAD_COPY_STACK_GUARD
  THREAD_COPY_STACK_GUARD(pd);
#endif
  tls_setup_tcbhead(pd);
  /* No clone, set_tid_address, rseq or robust-list syscall here. */
  atomic_fetch_add_relaxed(&__nptl_nthreads, 1);
  *token = pd;
  *thread_pointer = TLS_TPADJ(pd);
  *stack_top = (char *)stack + usable;
  return 0;
}

int __gw_domain_bind(void *token) {
  struct pthread *pd = THREAD_SELF;
  if (pd != token || !(pd->flags & ATTR_FLAG_GW_DOMAIN) || pd->tid != 0 ||
      pd->exiting) {
    return EINVAL;
  }
  pd->tid = INTERNAL_SYSCALL_CALL(gettid);
  /* setxid remains unavailable for borrowed carriers.  In particular, do
     not mark this as a normal clone participant in a credential broadcast. */
  __resp = &pd->res;
  __ctype_init();
  rseq_register_current_thread(pd, false);
  return 0;
}

int __gw_domain_run(void (*function)(void *), void *argument) {
  struct pthread *pd = THREAD_SELF;
  if (!(pd->flags & ATTR_FLAG_GW_DOMAIN) || pd->cleanup_jmp_buf != NULL) {
    return -EINVAL;
  }
  struct pthread_unwind_buf buffer;
  DIAG_PUSH_NEEDS_COMMENT;
  DIAG_IGNORE_NEEDS_COMMENT(11, "-Wstringop-overflow=");
  int unwound = setjmp((struct __jmp_buf_tag *)buffer.cancel_jmp_buf);
  DIAG_POP_NEEDS_COMMENT;
  buffer.priv.data.prev = NULL;
  buffer.priv.data.cleanup = NULL;
  THREAD_SETMEM(pd, cleanup_jmp_buf, &buffer);
  if (!unwound) {
    function(argument);
  }
  THREAD_SETMEM(pd, cleanup_jmp_buf, NULL);
  return unwound ? 1 : 0;
}

int __gw_domain_finish(void) {
  struct pthread *pd = THREAD_SELF;
  if (!(pd->flags & ATTR_FLAG_GW_DOMAIN) || pd->cleanup_jmp_buf != NULL ||
      pd->tid != INTERNAL_SYSCALL_CALL(gettid) || pd->exiting) {
    return EINVAL;
  }
  call_function_static_weak(__call_tls_dtors);
  __nptl_deallocate_tsd();
  __libc_thread_freeres();
  __getrandom_vdso_release(pd);
  __libc_lock_lock(pd->exit_lock);
  pd->exiting = true;
  __libc_lock_unlock(pd->exit_lock);
  /* The carrier remains alive.  Only the logical Guest domain terminates. */
  pd->tid = 0;
  return 0;
}

int __gw_domain_free(void *token) {
  struct pthread *pd = token;
  if (pd == NULL || pd == THREAD_SELF || !(pd->flags & ATTR_FLAG_GW_DOMAIN) ||
      pd->tid != 0 || !pd->exiting) {
    return EBUSY;
  }
  /* Owner calls only after carrier has switched back and acknowledged it.
     User-supplied stack storage is released separately by the supervisor. */
  atomic_fetch_add_relaxed(&__nptl_nthreads, -1);
  __nptl_free_tcb(pd);
  return 0;
}

/* Private ABI negotiated by the matching service before creating a domain.
 * Bump this whenever allocation, callback or lifecycle semantics change. */
unsigned long __gw_domain_capabilities(void) {
  return 0x4757440100000006UL;
}
