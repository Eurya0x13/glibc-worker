#ifndef GW_GUEST_FD_STATE_H
#define GW_GUEST_FD_STATE_H
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

struct epoll_file;
struct signal_fd;
struct gw_signal_targets;

struct gw_guest_fd_state {
  atomic_flag writer;
  struct gw_signal_targets *targets;
  struct signal_fd *descriptors;
  size_t capacity;
  _Atomic size_t count;
  _Atomic int layout;
  struct epoll_file *epolls;
  _Atomic size_t epoll_count;
  int monitor;
  uint64_t monitor_device, monitor_inode, epoll_revision;
  int64_t epoll_label;
  unsigned host_calls;
  int closed;
};

struct gw_guest_fd_state *gw_guest_fds_publish(struct gw_signal_targets *);
int gw_guest_fds_attach(struct gw_guest_fd_state *);
struct gw_guest_fd_state *gw_guest_fds_state(void);
int gw_guest_fds_active(void);
int gw_guest_readiness_active(void);
#endif
