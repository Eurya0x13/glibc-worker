#ifndef GW_RPC_SEQUENCE_H
#define GW_RPC_SEQUENCE_H
#include <limits.h>

/* One outstanding request per slot, acknowledged before reuse. Sequence
   words are equality tokens, never ordered counters. Wrap explicitly in the
   existing signed ABI; no signed overflow or implementation-defined cast. */
static inline int gw_rpc_next_sequence(int sequence) {
  return sequence == INT_MAX ? INT_MIN : sequence + 1;
}
#endif
