#pragma once
#include "domain_unwind.h"
#include "context-events.h"
struct jni_domain_context;
struct jni_domain_signal;
void gw_domain_checkpoint_enter(struct gw_domain_native_checkpoint *);
void gw_domain_checkpoint_leave(struct gw_domain_native_checkpoint *);
void gw_domain_jump_event(void *, int, int);
int gw_domain_context_event(enum gw_context_operation, ucontext_t *,
                            const ucontext_t *, const uint64_t *);
_Noreturn void gw_domain_restore_context(ucontext_t *);
int gw_domain_resume_signal(int, void *, void *);
void gw_domain_signal_select(struct jni_domain_signal *,
                             struct jni_domain_signal *);
void gw_domain_signal_retire_leaf(struct jni_domain_signal *,
                                  struct jni_domain_signal *);
void gw_domain_jump_signal_leave(struct jni_domain_signal *);
void gw_domain_jump_finish(void);
void gw_domain_jump_bind(struct jni_domain_context *);
int gw_domain_jump_init(void);
void gw_domain_rpc_finish(void);
