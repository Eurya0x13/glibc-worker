#include "signal_contract.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>

int main(void) {
  struct gw_signal_contract c = {
      .version = GW_SIGNAL_CONTRACT_V1,
      .size = sizeof c,
      .host_reserved =
          (UINT64_C(1) << (SIGSEGV - 1)) | (UINT64_C(1) << (SIGUSR2 - 1))};
  assert(!gw_signal_contract_validate(&c));
  unsigned checks = 0;
  for (int signal = 1; signal <= 64; ++signal) {
    enum gw_signal_domain owner = 0;
    int unavailable =
        signal == SIGKILL || signal == SIGSTOP || signal == 32 || signal == 33;
    int error = gw_signal_contract_owner(&c, signal, GW_SIGNAL_EXTERNAL,
                                         GW_SIGNAL_GUEST, &owner);
    assert(error == (unavailable ? EINVAL : 0));
    assert(owner == (unavailable ? 0 : GW_SIGNAL_HOST));
    ++checks;
    owner = 0;
    error = gw_signal_contract_owner(&c, signal, GW_SIGNAL_GUEST_TAGGED,
                                     GW_SIGNAL_HOST, &owner);
    int expected = unavailable                                       ? EINVAL
                   : c.host_reserved & (UINT64_C(1) << (signal - 1)) ? EBUSY
                                                                     : 0;
    assert(error == expected);
    assert(owner == (expected ? 0 : GW_SIGNAL_GUEST));
    ++checks;
  }
  const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
  for (unsigned i = 0; i < sizeof faults / sizeof faults[0]; ++i) {
    for (int domain = GW_SIGNAL_HOST; domain <= GW_SIGNAL_GUEST; ++domain) {
      enum gw_signal_domain owner = 0;
      assert(!gw_signal_contract_owner(&c, faults[i], GW_SIGNAL_SYNCHRONOUS,
                                       domain, &owner));
      assert(owner == (enum gw_signal_domain)domain);
      ++checks;
    }
  }
  enum gw_signal_domain owner = GW_SIGNAL_HOST;
  assert(gw_signal_contract_owner(&c, 0, GW_SIGNAL_EXTERNAL, GW_SIGNAL_HOST,
                                  &owner) == EINVAL);
  assert(owner == GW_SIGNAL_HOST);
  assert(gw_signal_contract_owner(&c, SIGUSR1, GW_SIGNAL_SYNCHRONOUS,
                                  GW_SIGNAL_GUEST, &owner) == EINVAL);
  assert(gw_signal_contract_owner(&c, SIGUSR1, 0, GW_SIGNAL_HOST, &owner) ==
         EINVAL);
  c.host_reserved |= UINT64_C(1) << (SIGKILL - 1);
  assert(gw_signal_contract_validate(&c) == EINVAL);
  c.host_reserved = 0;
  ++c.version;
  assert(gw_signal_contract_validate(&c) == EINVAL);
  printf("{\"result\":\"PASS\",\"checks\":%u}\n", checks + 5);
  return 0;
}
