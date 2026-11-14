#ifndef JNI_SIGNATURE_H
#define JNI_SIGNATURE_H
#include <ffi.h>
#include "full_protocol.h"

/* Parse a JNI method descriptor into argument type chars plus the return
   char. Arrays collapse to 'L' because they marshal as pointers. Rejects
   malformed or unknown descriptors and anything beyond FB_ARGS args. */
static inline int sig_types(const char *s, char *types, int *count, char *ret) {
  if (!s || *s++ != '(') {
    return 0;
  }

  int n = 0;
  while (*s && *s != ')') {
    if (n >= FB_ARGS) {
      return 0;
    }
    char t = *s++;
    if (t == '[') {
      while (*s == '[') {
        s++;
      }
      if (*s == 'L') {
        while (*s && *s != ';') {
          s++;
        }
        if (!*s) {
          return 0;
        }
      }
      if (!*s) {
        return 0;
      }
      s++;
      t = 'L';
    } else if (t == 'L') {
      while (*s && *s != ';') {
        s++;
      }
      if (!*s) {
        return 0;
      }
      s++;
    } else if (!strchr("ZBCSIJFD", t)) {
      return 0;
    }
    types[n++] = t;
  }

  if (*s++ != ')') {
    return 0;
  }
  char t = *s;
  if (t == '[') {
    t = 'L';
  }
  if (!strchr("VZBCSIJFDL", t)) {
    return 0;
  }

  *count = n;
  *ret = t;
  return 1;
}

/* Descriptor char -> libffi type. Every reference form (objects and arrays)
   marshals as the platform pointer. */
static inline ffi_type *jni_ffi_type(char t) {
  switch (t) {
    case 'V':
      return &ffi_type_void;
    case 'Z':
      return &ffi_type_uint8;
    case 'B':
      return &ffi_type_sint8;
    case 'C':
      return &ffi_type_uint16;
    case 'S':
      return &ffi_type_sint16;
    case 'I':
      return &ffi_type_sint32;
    case 'J':
      return &ffi_type_sint64;
    case 'F':
      return &ffi_type_float;
    case 'D':
      return &ffi_type_double;
    default:
      return &ffi_type_pointer;
  }
}
#endif
