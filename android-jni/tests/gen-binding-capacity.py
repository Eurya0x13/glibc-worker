#!/usr/bin/env python3
"""Generate distinct native methods for the real-VM binding-capacity test."""

import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    java = args.output / "java/conformance"
    java.mkdir(parents=True, exist_ok=True)
    count = 1024
    declarations = [
        f"  private static native int method{i}(int value);"
        for i in range(count)
    ]
    calls = [
        f'    if (method{i}(17) != {17 + i}) throw new AssertionError("binding {i}");'
        for i in range(count)
    ]
    (java / "BindingCapacity.java").write_text(
        "package conformance;\npublic final class BindingCapacity {\n"
        "  public static boolean enabled;\n"
        + "\n".join(declarations)
        + "\n  public static int verify() {\n"
        + "\n".join(calls)
        + f"\n    return {count};\n  }}\n}}\n"
    )
    resource_classes = [
        f"  public static final class Class{i} {{ public static native int probe(int value); }}"
        for i in range(320)
    ]
    resource_calls = [
        f'    if (Class{i}.probe(31) != 31) throw new AssertionError("class {i}");'
        for i in range(320)
    ]
    short_type, long_type = "B" * 221, "A" * 230
    prefix = "conformance/ResourceCapacity$"
    assert len("(L" + prefix + short_type + ";)V") == 255
    assert len("L" + prefix + long_type + ";") > 255
    descriptor_members = (
        f"  public static final class {short_type} {{}}\n"
        f"  public static final class {long_type} {{}}\n"
        f"  private static void accepted({short_type} value) {{}}\n"
        f"  private static void rejected({long_type} value) {{}}\n"
        f"  private static {long_type} rejectedField;\n"
        "  private static native boolean descriptorProbe(Object member, boolean field);\n"
        "  public static int descriptorChecks;\n"
        "  public static native int failureProbe(int kind, int depth);\n"
        "  public static int recurse(int kind, int depth) { return failureProbe(kind, depth); }\n"
    )
    descriptor_calls = (
        f'    if (!descriptorProbe(ResourceCapacity.class.getDeclaredMethod("accepted", {short_type}.class), false)) throw new AssertionError("255-byte method");\n'
        f'    try {{ descriptorProbe(ResourceCapacity.class.getDeclaredMethod("rejected", {long_type}.class), false); throw new AssertionError("method truncation"); }} catch (IllegalArgumentException expected) {{}}\n'
        '    try { descriptorProbe(ResourceCapacity.class.getDeclaredField("rejectedField"), true); throw new AssertionError("field truncation"); } catch (IllegalArgumentException expected) {}\n'
        "    descriptorChecks = 3;\n"
    )
    (java / "ResourceCapacity.java").write_text(
        "package conformance;\npublic final class ResourceCapacity {\n"
        "  public static int references;\n"
        + "\n".join(resource_classes)
        + "\n"
        + descriptor_members
        + "\n  public static int verify() throws Exception {\n"
        + "\n".join(resource_calls)
        + "\n"
        + descriptor_calls
        + '\n    if (references != 20000) throw new AssertionError("reference capacity");\n'
        + "    return 320;\n  }\n}\n"
    )
    native = [
        f"static jint binding_method{i}(JNIEnv *e, jclass c, jint v) "
        + f"{{ (void)e; (void)c; return v + {i}; }}"
        for i in range(count)
    ]
    native.append("static const JNINativeMethod capacity_methods[] = {")
    native += [
        f'  {{"method{i}", "(I)I", (void *)binding_method{i}}},'
        for i in range(count)
    ]
    native.append("};")
    native.append("""
/* Each nested class has a real VM registration, but shares the same Guest
   target/signature. This stresses class ownership separately from closures. */
static jboolean descriptor_probe(JNIEnv *e, jclass cls, jobject member, jboolean field) {
  (void)cls;
  return field ? ((*e)->FromReflectedField(e, member) != NULL) :
                 ((*e)->FromReflectedMethod(e, member) != NULL);
}

static int resource_capacity_register(JNIEnv *e) {
  JNINativeMethod probe = {"probe", "(I)I", (void *)binding_method0};
  for (int i = 0; i < 320; i++) {
    char name[96];
    snprintf(name, sizeof name, "conformance/ResourceCapacity$Class%d", i);
    jclass cls = (*e)->FindClass(e, name);
    if (!cls) return JNI_ERR;
    jint result = (*e)->RegisterNatives(e, cls, &probe, 1);
    (*e)->DeleteLocalRef(e, cls);
    if (result != JNI_OK) return JNI_ERR;
  }
  jclass cls = (*e)->FindClass(e, "conformance/ResourceCapacity");
  if (!cls) return JNI_ERR;
  JNINativeMethod descriptor = {"descriptorProbe", "(Ljava/lang/Object;Z)Z", (void *)descriptor_probe};
  if ((*e)->RegisterNatives(e, cls, &descriptor, 1) != JNI_OK) return JNI_ERR;
  JNINativeMethod failure = {"failureProbe", "(II)I", (void *)failure_probe};
  if ((*e)->RegisterNatives(e, cls, &failure, 1) != JNI_OK) return JNI_ERR;
  jobject *refs = calloc(20000, sizeof *refs);
  if (!refs) { (*e)->DeleteLocalRef(e, cls); return JNI_ERR; }
  int count = 0;
  while (count < 20000 && (refs[count] = (*e)->NewGlobalRef(e, cls))) count++;
  int ok = count == 20000 && (*e)->IsSameObject(e, refs[0], refs[19999]);
  while (count) (*e)->DeleteGlobalRef(e, refs[--count]);
  free(refs);
  if (ok) {
    jfieldID field = (*e)->GetStaticFieldID(e, cls, "references", "I");
    if (field) (*e)->SetStaticIntField(e, cls, field, 20000);
    else ok = 0;
  }
  (*e)->DeleteLocalRef(e, cls);
  return ok && !(*e)->ExceptionCheck(e) ? JNI_OK : JNI_ERR;
}

static int binding_capacity_register(JNIEnv *e) {
  if (resource_capacity_register(e) != JNI_OK) return JNI_ERR;
  jclass cls = (*e)->FindClass(e, "conformance/BindingCapacity");
  if (!cls) return JNI_ERR;
  jmethodID first = (*e)->GetStaticMethodID(e, cls, "method0", "(I)I");
  if (!first) return JNI_ERR;
  for (int start = 0; start < 1024; start += 64) {
    if ((*e)->RegisterNatives(e, cls, capacity_methods + start, 64) != JNI_OK)
      return JNI_ERR;
    jint value = (*e)->CallStaticIntMethod(e, cls, first, 17);
    if ((*e)->ExceptionCheck(e) || value != 17) return JNI_ERR;
  }
  jfieldID enabled = (*e)->GetStaticFieldID(e, cls, "enabled", "Z");
  if (!enabled) return JNI_ERR;
  (*e)->SetStaticBooleanField(e, cls, enabled, JNI_TRUE);
  (*e)->DeleteLocalRef(e, cls);
  return (*e)->ExceptionCheck(e) ? JNI_ERR : JNI_OK;
}
""")
    (args.output / "binding_capacity_generated.inc").write_text(
        "\n".join(native) + "\n"
    )


if __name__ == "__main__":
    main()
