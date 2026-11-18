#ifndef GW_TEST_GUEST_ASYNC_HOST_IO_H
#define GW_TEST_GUEST_ASYNC_HOST_IO_H
#include "common.h"

void check_host_io(struct async_carrier *);
long host_io_call_number(int phase);
void host_io_unblock(struct async_carrier *, int phase);
#endif
