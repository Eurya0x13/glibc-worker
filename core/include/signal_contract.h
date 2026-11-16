#ifndef GW_SIGNAL_CONTRACT_H
#define GW_SIGNAL_CONTRACT_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Domain labels describe the interrupted instruction, not the signal sender.
 * A synchronous fault overrides asynchronous reservations: a Guest fault must
 * never enter the VM's implicit-exception handler. */
enum gw_signal_domain { GW_SIGNAL_HOST = 1, GW_SIGNAL_GUEST = 2 };

enum gw_signal_source {
  GW_SIGNAL_EXTERNAL = 1,
  GW_SIGNAL_GUEST_TAGGED = 2,
  GW_SIGNAL_SYNCHRONOUS = 3
};

struct gw_signal_contract {
  uint32_t version;
  uint32_t size;
  uint64_t host_reserved;
};

#define GW_SIGNAL_CONTRACT_V1 1u

/* Returns an errno value. The contract must remain immutable while in use.
 * SIGKILL, SIGSTOP and NPTL's private signals cannot be virtualized. */
int gw_signal_contract_validate(const struct gw_signal_contract *);

/* Pure, TLS-free classification. Returns zero and writes owner, or an errno
 * value without modifying owner. A tagged Guest send to a Host-reserved signal
 * is rejected with EBUSY; it is never converted into a signal to the VM.
 * Classification alone does not establish handler/mask virtualization. */
int gw_signal_contract_owner(const struct gw_signal_contract *, int number,
                             enum gw_signal_source,
                             enum gw_signal_domain executing,
                             enum gw_signal_domain *owner);
#ifdef __cplusplus
}
#endif
#endif
