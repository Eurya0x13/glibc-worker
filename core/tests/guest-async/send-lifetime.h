#ifndef GW_TEST_SEND_LIFETIME_H
#define GW_TEST_SEND_LIFETIME_H
#include <signal.h>

typedef int (*test_signal_sender)(void *, int, const siginfo_t *, int *);
void test_send_lifetimes(int (*)(unsigned, test_signal_sender),
                         int (*)(void (*)(int)), test_signal_sender);
int test_send_lifetime_probe(void *, int, const siginfo_t *, int *);
#endif
