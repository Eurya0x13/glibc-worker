#ifndef GW_SIGNAL_USER_COPY_H
#define GW_SIGNAL_USER_COPY_H
#include <stddef.h>

/* Bootstrap prepares one process-owned, preallocated scratch OFD. Every copy
 * physically blocks catchable signals and holds its short writer. Raw kernel
 * I/O preserves EFAULT/partial-copy behavior without touching caller memory in
 * C. The application must not close or replace this runtime-owned descriptor.
 * An absent/different descriptor in a private FD table fails with ENOTSUP. */
int gw_signal_user_copy_prepare(void);
int gw_signal_user_copy_in(void *, const void *, size_t);
int gw_signal_user_copy_out(void *, const void *, size_t);
#endif
