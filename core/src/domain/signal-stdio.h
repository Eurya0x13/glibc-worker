#ifndef GW_SIGNAL_STDIO_H
#define GW_SIGNAL_STDIO_H

/* Optional deferred-channel integration, not part of regular JNI calls.
 * Activation/restoration requires quiescent Host stdio users. The adapter
 * preserves native FILE objects, ownership, buffering, fileno and freopen.
 * Unsupported Host paths fail before enabling virtual signal descriptors.
 * Direct libc handles/raw syscalls still bypass public bootstrap routing. */
int signal_stdio_enable(void);
int signal_stdio_verify(void);
int signal_stdio_disable(void);
const char *signal_stdio_profile(void);
#endif
