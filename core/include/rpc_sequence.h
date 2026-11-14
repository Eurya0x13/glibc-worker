#ifndef GW_RPC_SEQUENCE_H
#define GW_RPC_SEQUENCE_H
#include <limits.h>

/* Sequence tokens pair the request and response words of one RPC slot.
   Publishing a request advances the slot's request word with
   gw_rpc_next_sequence; the peer acknowledges by copying the observed
   request word into the response word; equal words mean the slot is idle.
   Holders compare tokens for equality only - they wrap through the whole
   signed range, so order or distance between two tokens is meaningless.
   Zero is an ordinary token: idleness is expressed by equal words, never
   by a reserved sentinel. */

/* One outstanding request per slot, acknowledged before reuse. Sequence
   words are equality tokens, never ordered counters. Wrap explicitly in the
   existing signed ABI; no signed overflow or implementation-defined cast. */
static inline int gw_rpc_next_sequence(int sequence) {
  return sequence == INT_MAX ? INT_MIN : sequence + 1;
}
#endif
