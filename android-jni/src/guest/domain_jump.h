#pragma once
#include "domain_unwind.h"
#include <ucontext.h>
struct jni_domain_context;
struct jni_domain_signal;
void gw_domain_checkpoint_enter(struct gw_domain_native_checkpoint *);
void gw_domain_checkpoint_leave(struct gw_domain_native_checkpoint *);
void gw_domain_jump_event(void *, int, int);
int gw_domain_context_event(ucontext_t *, const ucontext_t *, int resumable);
_Noreturn void gw_domain_restore_context(ucontext_t *);
int gw_domain_resume_signal(int, void *, void *);
void gw_domain_jump_signal_leave(struct jni_domain_signal *);
void gw_domain_jump_finish(void);
void gw_domain_jump_bind(struct jni_domain_context *);
int gw_domain_jump_init(void);
void gw_domain_rpc_finish(void);
