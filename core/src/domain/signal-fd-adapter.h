#ifndef GW_SIGNAL_FD_ADAPTER_H
#define GW_SIGNAL_FD_ADAPTER_H
#include <stddef.h>
#include <sys/types.h>
#include "signalfd-hook.h"

typedef ssize_t (*gw_signal_fd_reader)(void *, void *, size_t);
/* Retain the owning libc's actual backend, including cancellation policy and
 * FILE offset bookkeeping. Translate only actual virtual signalfd objects. */
ssize_t gw_signal_fd_read_backend(int, void *, size_t, gw_signal_fd_reader,
                                  void *);
ssize_t gw_signal_fd_read_policy(int, void *, size_t, gw_signal_fd_reader,
                                 void *, enum gw_libc_io_policy);
#endif
