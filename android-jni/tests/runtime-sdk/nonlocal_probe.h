#ifndef SDK_NONLOCAL_PROBE_H
#define SDK_NONLOCAL_PROBE_H
/* Fixture-owned realtime signal, checked unused by the Host after VM startup.
 * Keep VM-owned signals intact while testing an external Host-directed send. */
#define SDK_HOST_ESCAPE_SIGNAL 52
int sdk_guest_fault_probe(int);
int sdk_guest_stack_probe(void *, void *, int);
int sdk_guest_wait_probe(int);
int sdk_guest_registration_probe(void);
#endif
