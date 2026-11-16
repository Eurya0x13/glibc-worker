/* Stable paged registry. Pages live until process exit; records are reused
 * after their owning thread has revoked delivery. Readers never allocate,
 * acquire a lock, dereference TLS, or follow a pointer into a freed TCB. */
struct signal_record {
  struct signal_slot slot;
  _Atomic int busy;
  int guest;
};

struct signal_page {
  struct signal_page *next;
  struct signal_record records[16];
};

static _Atomic(struct signal_page *) signal_pages;

#define TLS_FREE __attribute__((no_stack_protector))

static TLS_FREE long registry_map(size_t size) {
#if defined(__aarch64__)
  register long a0 __asm__("x0") = 0;
  register long a1 __asm__("x1") = size;
  register long a2 __asm__("x2") = PROT_READ | PROT_WRITE;
  register long a3 __asm__("x3") = MAP_PRIVATE | MAP_ANONYMOUS;
  register long a4 __asm__("x4") = -1;
  register long a5 __asm__("x5") = 0;
  register long nr __asm__("x8") = SYS_mmap;
  __asm__ volatile("svc #0"
                   : "+r"(a0)
                   : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(nr)
                   : "memory", "cc");
  return a0;
#else
  register long a3 __asm__("r10") = MAP_PRIVATE | MAP_ANONYMOUS;
  register long a4 __asm__("r8") = -1;
  register long a5 __asm__("r9") = 0;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(SYS_mmap), "D"(0L), "S"(size),
                     "d"(PROT_READ | PROT_WRITE), "r"(a3), "r"(a4), "r"(a5)
                   : "rcx", "r11", "memory");
  return result;
#endif
}

static TLS_FREE struct signal_record *registry_claim(void) {
  struct signal_page *head =
      atomic_load_explicit(&signal_pages, memory_order_acquire);
  for (struct signal_page *p = head; p; p = p->next) {
    for (unsigned i = 0; i < 16; ++i) {
      int free = 0;
      if (atomic_compare_exchange_strong(&p->records[i].busy, &free, 1)) {
        return &p->records[i];
      }
    }
  }
  long address = registry_map(sizeof(struct signal_page));
  if ((unsigned long)address >= (unsigned long)-4095) {
    return NULL;
  }
  struct signal_page *p = (void *)address;
  atomic_store(&p->records[0].busy, 1);
  do {
    p->next = head;
  } while (!atomic_compare_exchange_weak_explicit(
      &signal_pages, &head, p, memory_order_release, memory_order_acquire));
  return &p->records[0];
}

TLS_FREE struct signal_slot *signal_dynamic_lookup(long tid, uintptr_t tp) {
  struct signal_page *p =
      atomic_load_explicit(&signal_pages, memory_order_acquire);
  for (; p; p = p->next) {
    for (unsigned i = 0; i < 16; ++i) {
      struct signal_record *r = &p->records[i];
      if (atomic_load_explicit(&r->slot.tid, memory_order_acquire) == tid) {
        if (!r->guest) {
          return &r->slot;
        }
        if (r->slot.host_tp == tp) {
          return (struct signal_slot *)(uintptr_t)-1;
        }
      }
    }
  }
  return NULL;
}

struct signal_slot *signal_acquire(struct exchange *e) {
  struct signal_record *r = registry_claim();
  if (!r) {
    return NULL;
  }
  r->guest = 0;
  atomic_store(&r->slot.failures, 0);
  atomic_store(&r->slot.signals, 0);
  atomic_store(&r->slot.faults, 0);
  if (signal_try_register(&r->slot, e)) {
    atomic_store(&r->busy, 0);
    return NULL;
  }
  publish_recipient(&r->slot);
  return &r->slot;
}

void signal_release(struct signal_slot *slot) {
  struct signal_record *r = (void *)slot;
  signal_unregister(slot);
  if (atomic_load(&slot->failures)) {
    _exit(98);
  }
  atomic_store(&r->busy, 0);
}

/* Called by patched Guest NPTL with all application signals blocked, before
 * entering user code and after TLS destructors on exit. No Host libc calls. */
TLS_FREE void signal_guest_lifecycle(int entering) {
  long tid = gw_raw3(SYS_gettid, 0, 0, 0);
  uintptr_t tp = (uintptr_t)__builtin_thread_pointer();
  if (entering) {
    struct signal_record *r = registry_claim();
    if (!r) {
      gw_raw3(SYS_exit_group, 98, 0, 0);
      __builtin_unreachable();
    }
    r->guest = 1;
    r->slot.host_tp = tp;
    atomic_store_explicit(&r->slot.tid, tid, memory_order_release);
    return;
  }
  struct signal_page *p =
      atomic_load_explicit(&signal_pages, memory_order_acquire);
  for (; p; p = p->next) {
    for (unsigned i = 0; i < 16; ++i) {
      struct signal_record *r = &p->records[i];
      if (atomic_load_explicit(&r->slot.tid, memory_order_acquire) == tid &&
          r->guest && r->slot.host_tp == tp) {
        atomic_store_explicit(&r->slot.tid, 0, memory_order_release);
        atomic_store(&r->busy, 0);
        return;
      }
    }
  }
  gw_raw3(SYS_exit_group, 98, 0, 0);
  __builtin_unreachable();
}

#undef TLS_FREE
