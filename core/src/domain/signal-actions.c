#define _GNU_SOURCE
#include "signal-actions.h"
#include <errno.h>
#include <string.h>

static void lock(struct gw_signal_actions *actions) {
  while (atomic_flag_test_and_set_explicit(&actions->writer,
                                           memory_order_acquire)) {
  }
}

static void unlock(struct gw_signal_actions *actions) {
  atomic_flag_clear_explicit(&actions->writer, memory_order_release);
}

static int validate(struct gw_signal_actions *actions, int number) {
  if (number <= 0 || number > 64 || number == SIGKILL || number == SIGSTOP ||
      number == 32 || number == 33) {
    return EINVAL;
  }
  if (actions->reserved & (UINT64_C(1) << (number - 1))) {
    return EBUSY;
  }
  return actions->closed ? ESRCH : 0;
}

static int ignored(int number, const struct sigaction *action) {
  return action->sa_handler == SIG_IGN ||
         (action->sa_handler == SIG_DFL &&
          (number == SIGCHLD || number == SIGURG || number == SIGWINCH ||
           number == SIGCONT));
}

void gw_signal_actions_init(struct gw_signal_actions *actions,
                            struct gw_signal_targets *targets,
                            uint64_t reserved) {
  memset(actions, 0, sizeof *actions);
  atomic_flag_clear(&actions->writer);
  actions->targets = targets;
  actions->reserved = reserved;
}

int gw_signal_actions_representative(struct gw_signal_actions *actions,
                                     struct gw_signal_target *target) {
  if (!target || target->registry != actions->targets) {
    return EINVAL;
  }
  lock(actions);
  int error = actions->closed           ? ESRCH
              : actions->representative ? EALREADY
                                        : gw_signal_target_pin(target);
  if (!error) {
    actions->representative = target;
  }
  unlock(actions);
  return error;
}

int gw_signal_actions_change(struct gw_signal_actions *actions, int number,
                             const struct sigaction *input,
                             struct sigaction *previous) {
  /* Preserve the native act == oldact case. No application pointer is stored
   * for later use by the registry or a signal recipient. */
  struct sigaction next;
  if (input) {
    next = *input;
  }

  lock(actions);
  int error = validate(actions, number);
  if (!error && input && actions->revision == UINT64_MAX) {
    error = EOVERFLOW;
  }
  if (!error && input && actions->targets && ignored(number, &next) &&
      !(actions->targets->inbox.reserved & (UINT64_C(1) << (number - 1)))) {
    /* Linux do_sigaction removes already-pending ignored signals, including
     * default-ignored dispositions. Serialize future sends with this flush.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
    error = gw_signal_targets_discard(actions->targets, number);
  }
  if (!error) {
    if (previous) {
      *previous = actions->current[number].action;
    }
    if (input) {
      actions->current[number] =
          (struct gw_signal_action){next, ++actions->revision};
    }
  }
  unlock(actions);
  return error;
}

static void select_action(struct gw_signal_actions *actions, int number,
                          struct gw_signal_action *selected) {
  *selected = actions->current[number];
  if (selected->action.sa_handler != SIG_DFL &&
      selected->action.sa_handler != SIG_IGN &&
      (selected->action.sa_flags & SA_RESETHAND)) {
    /* Reset only the handler. Flags, mask and registration identity remain
     * those of this selected registration, matching Linux handler selection. */
    actions->current[number].action.sa_handler = SIG_DFL;
  }
}

int gw_signal_actions_select(struct gw_signal_actions *actions, int number,
                             struct gw_signal_action *selected) {
  if (!selected) {
    return EINVAL;
  }
  lock(actions);
  int error = validate(actions, number);
  if (!error) {
    select_action(actions, number, selected);
  }
  unlock(actions);
  return error;
}

struct gw_signal_publication gw_signal_actions_send(
    struct gw_signal_actions *actions, uintptr_t key, int tid,
    const siginfo_t *information) {
  if (!information || !actions->targets) {
    return (struct gw_signal_publication){.status = EINVAL};
  }

  lock(actions);
  int number = information->si_signo;
  struct gw_signal_publication result = {0};
  result.status = number            ? validate(actions, number)
                  : actions->closed ? ESRCH
                                    : 0;
  if (!result.status && number && information->si_code > 0) {
    result.status = EINVAL;
  }
  if (!result.status && number &&
      (actions->targets->inbox.reserved & (UINT64_C(1) << (number - 1)))) {
    result.status = EBUSY;
  }
  if (!result.status && !key && !actions->representative) {
    result.status = ENOTSUP;
  }
  if (!result.status) {
    /* Linux sig_ignored tests the target's blocked and real_blocked masks,
     * even for SIG_IGN. Process sends use the group leader's mask, not the
     * recipient later chosen by complete_signal.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
    result = gw_signal_targets_send_filtered(
        actions->targets, key, tid, information,
        number && ignored(number, &actions->current[number].action),
        actions->representative);
  }
  unlock(actions);
  return result;
}

int gw_signal_actions_deliver(struct gw_signal_actions *actions,
                              struct gw_signal_target *target,
                              struct gw_signal_delivery *delivery) {
  if (!delivery || !target || target->registry != actions->targets) {
    return -EINVAL;
  }
  lock(actions);
  int result = actions->closed ? -ESRCH : 0;
  if (!result) {
    result = gw_signal_target_deliver(target, &delivery->information,
                                      &delivery->source);
    if (result > 0) {
      select_action(actions, result, &delivery->selected);
    }
  }
  unlock(actions);
  return result;
}

void gw_signal_actions_close(struct gw_signal_actions *actions) {
  lock(actions);
  actions->closed = 1;
  if (actions->representative) {
    gw_signal_target_unpin(actions->representative);
    actions->representative = NULL;
  }
  unlock(actions);
}
