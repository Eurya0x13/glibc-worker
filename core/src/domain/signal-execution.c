#define _GNU_SOURCE
#include "signal-execution.h"
#include "signal-stack.h"
#include <string.h>

static int valid_stack(void *address, size_t size) {
  return address && size >= 16384 && size <= UINTPTR_MAX - (uintptr_t)address;
}

static int overlaps(void *first, size_t first_size, void *second,
                    size_t second_size) {
  uintptr_t a = (uintptr_t)first, b = (uintptr_t)second;
  return a < b + second_size && b < a + first_size;
}

static int validate_stacks(const struct gw_signal_execution_config *config) {
  if (!valid_stack(config->stack, config->stack_size) ||
      !valid_stack(config->abort_stack, config->abort_stack_size) ||
      overlaps(config->stack, config->stack_size, config->abort_stack,
               config->abort_stack_size)) {
    return EINVAL;
  }
  const stack_t *alternate = &config->alternate;
  if (alternate->ss_flags == SS_DISABLE) {
    return 0;
  }
  if ((alternate->ss_flags &&
       (unsigned)alternate->ss_flags != GW_SS_AUTODISARM) ||
      !alternate->ss_sp || !alternate->ss_size ||
      alternate->ss_size > UINTPTR_MAX - (uintptr_t)alternate->ss_sp ||
      overlaps(alternate->ss_sp, alternate->ss_size, config->abort_stack,
               config->abort_stack_size)) {
    return EINVAL;
  }
  return 0;
}

static void set_entry(struct domain *state, uintptr_t top, uintptr_t start,
                      void *argument, void (*entry)(void *)) {
  state->sp = top & ~(uintptr_t)15;
  state->pc = start;
#if defined(__x86_64__)
  state->saved[2] = (uintptr_t)argument;
  state->saved[3] = (uintptr_t)entry;
#else
  state->saved[0] = (uintptr_t)argument;
  state->saved[1] = (uintptr_t)entry;
#endif
}

int gw_signal_execution_from_saved(
    struct gw_signal_execution *execution,
    const struct gw_signal_execution_config *config, const struct domain *saved,
    const struct gw_signal_execution_entries *entries) {
  if (!execution || !config || !config->swap || !config->invoke ||
      !config->finish || !saved || !saved->tp || !entries || !entries->start ||
      !entries->run || !entries->abandon || validate_stacks(config)) {
    return EINVAL;
  }

  memset(execution, 0, sizeof *execution);
  execution->swap = config->swap;
  execution->invoke = config->invoke;
  execution->finish = config->finish;
  execution->release_host = config->release_host;
  execution->argument = config->argument;
  execution->carrier_mask = config->carrier_mask;
  execution->alternate = config->alternate;
  execution->version = 1;
  execution->guest = execution->abort = *saved;
  set_entry(&execution->guest, (uintptr_t)config->stack + config->stack_size,
            entries->start, execution, entries->run);
  set_entry(&execution->abort,
            (uintptr_t)config->abort_stack + config->abort_stack_size,
            entries->start, execution, entries->abandon);
  execution->phase = GW_SIGNAL_EXECUTION_PREPARED;
  return 0;
}
