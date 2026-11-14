#ifndef GW_BOOTSTRAP_ARCH_H
#define GW_BOOTSTRAP_ARCH_H
#include <elf.h>
/* Single build-architecture selection shared by bootstrap and diagnostics.
 * GW_ELF_MACHINE validates the e_machine of loader images at map time and
 * rejects a loader built for another architecture. GW_ARCH_NAME is the
 * human-readable label for diagnostics; GW_LOADER_NAME is the file name the
 * bionic-side ELF classifier uses to recognize the pinned glibc dynamic
 * loader. All participants must compile with the same selection; there is no
 * cross-arch fallback. */
#if defined(__aarch64__) || defined(GW_CLASSIFY_AARCH64)
#define GW_ARCH_NAME "arm64-v8a"
#define GW_ELF_MACHINE EM_AARCH64
#define GW_LOADER_NAME "ld-linux-aarch64.so.1"
#elif defined(__x86_64__)
#define GW_ARCH_NAME "x86_64"
#define GW_ELF_MACHINE EM_X86_64
#define GW_LOADER_NAME "ld-linux-x86-64.so.2"
#else
#error Unsupported target architecture
#endif
#endif
