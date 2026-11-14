#include <stdint.h>

struct packet {
  int32_t value;
  int32_t callback_value;
  uint64_t guest_tp;
};

typedef int (*callback_t)(struct packet *);
typedef int (*probe_t)(struct packet *, callback_t);
