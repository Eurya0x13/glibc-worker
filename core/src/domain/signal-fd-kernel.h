#ifndef GW_SIGNAL_FD_KERNEL_H
#define GW_SIGNAL_FD_KERNEL_H
#include <stdint.h>

/* Kernel-authoritative descriptor identity. No cache indexed only by an FD:
 * numbers can be imported, reused, closed in ranges or private to a thread.
 * Return 1 for signalfd, 0 for another/closed object, -1 with errno on failure.
 * Descriptor owners serialize replacement against reads and mode changes. */
int gw_signal_fd_kernel(int descriptor, uint64_t *mask);
/* Bootstrap/restore only, with descriptor owners quiescent. Includes private
 * files_struct instances through /proc/self/task/TID/fd{,info}. A zero filter
 * finds any signalfd; otherwise find a mask intersecting filter. Returns an
 * errno value, including EBUSY for a retained object and ENOTSUP when
 * inspection is unavailable. No allocation, libc directory stream or
 * cancellation point. */
int gw_signal_fd_inventory(uint64_t filter);
#endif
