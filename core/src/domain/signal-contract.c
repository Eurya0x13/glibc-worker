#include "signal_contract.h"
#include <errno.h>
#include <signal.h>

static uint64_t signal_bit(int number) {
  return UINT64_C(1) << (number - 1);
}

int gw_signal_contract_validate(const struct gw_signal_contract *contract) {
  if (!contract || contract->version != GW_SIGNAL_CONTRACT_V1 ||
      contract->size != sizeof *contract) {
    return EINVAL;
  }
  uint64_t unavailable = signal_bit(SIGKILL) | signal_bit(SIGSTOP);
  return contract->host_reserved & unavailable ? EINVAL : 0;
}

/* No libc calls or TLS accesses are allowed before restoring the selected
 * domain's thread pointer. Keep this path usable from the assembly router. */
__attribute__((no_stack_protector)) int gw_signal_contract_owner(
    const struct gw_signal_contract *contract, int number,
    enum gw_signal_source source, enum gw_signal_domain executing,
    enum gw_signal_domain *owner) {
  if (!owner || number < 1 || number > 64 || number == SIGKILL ||
      number == SIGSTOP || number == 32 || number == 33) {
    return EINVAL;
  }
  int error = gw_signal_contract_validate(contract);
  if (error) {
    return error;
  }
  if (source == GW_SIGNAL_SYNCHRONOUS) {
    if ((number != SIGSEGV && number != SIGBUS && number != SIGILL &&
         number != SIGFPE && number != SIGTRAP && number != SIGSYS) ||
        (executing != GW_SIGNAL_HOST && executing != GW_SIGNAL_GUEST)) {
      return EINVAL;
    }
    *owner = executing;
    return 0;
  }
  if (source == GW_SIGNAL_EXTERNAL) {
    *owner = GW_SIGNAL_HOST;
    return 0;
  }
  if (source == GW_SIGNAL_GUEST_TAGGED) {
    if (contract->host_reserved & signal_bit(number)) {
      return EBUSY;
    }
    *owner = GW_SIGNAL_GUEST;
    return 0;
  }
  return EINVAL;
}
