package dev.glibc.worker;

/** VM-neutral reflection helpers for the JNI bridge, independent of test fixtures. */
public final class BridgeTypes {
  private BridgeTypes() {}

  /**
   * Returns the declaring class of a reflected member. Called from native code, which cannot
   * walk java.lang.reflect itself.
   */
  public static Class<?> declaringClass(Object member) {
    return ((java.lang.reflect.Member) member).getDeclaringClass();
  }

  /**
   * Maps a Class to its JNI field descriptor; arrays keep their JVM encoding with separators
   * converted to '/'.
   */
  private static String descriptor(Class<?> cls) {
    if (cls.isArray()) {
      return cls.getName().replace('.', '/');
    }
    if (!cls.isPrimitive()) {
      return "L" + cls.getName().replace('.', '/') + ";";
    }

    if (cls == void.class) {
      return "V";
    }
    if (cls == boolean.class) {
      return "Z";
    }
    if (cls == byte.class) {
      return "B";
    }
    if (cls == char.class) {
      return "C";
    }
    if (cls == short.class) {
      return "S";
    }
    if (cls == int.class) {
      return "I";
    }
    if (cls == long.class) {
      return "J";
    }
    if (cls == float.class) {
      return "F";
    }
    return "D";
  }

  /**
   * Builds the JNI member descriptor the native bridge parses: a field yields its type
   * descriptor, while methods and constructors yield "(params)return". Called from native code
   * via fh_describe, so the grammar must stay in sync with sig_types.
   */
  public static String describeMember(Object member) {
    if (member instanceof java.lang.reflect.Field) {
      return descriptor(((java.lang.reflect.Field) member).getType());
    }

    Class<?>[] params;
    Class<?> result;
    if (member instanceof java.lang.reflect.Method) {
      java.lang.reflect.Method m = (java.lang.reflect.Method) member;
      params = m.getParameterTypes();
      result = m.getReturnType();
    } else {
      params = ((java.lang.reflect.Constructor<?>) member).getParameterTypes();
      result = void.class;
    }

    StringBuilder s = new StringBuilder("(");
    for (Class<?> p : params) {
      s.append(descriptor(p));
    }
    return s.append(')').append(descriptor(result)).toString();
  }
}
