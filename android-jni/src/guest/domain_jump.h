#pragma once
#include "domain_unwind.h"
struct jni_domain_context;
struct jni_domain_signal;
void gw_domain_checkpoint_enter(struct gw_domain_native_checkpoint *);
void gw_domain_checkpoint_leave(struct gw_domain_native_checkpoint *);
void gw_domain_jump_event(void *, int, int);
void gw_domain_jump_signal_leave(struct jni_domain_signal *);
void gw_domain_jump_finish(void);
void gw_domain_jump_bind(struct jni_domain_context *);
int gw_domain_jump_init(void);
void gw_domain_rpc_finish(void);
