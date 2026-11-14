#ifndef GW_TEST_MUTEX_CONTRACT_H
#define GW_TEST_MUTEX_CONTRACT_H

enum gw_mutex_expectation { GW_MUTEX_REJECTED, GW_MUTEX_RECOVERED };

int gw_mutex_contract(enum gw_mutex_expectation expected);

#endif
