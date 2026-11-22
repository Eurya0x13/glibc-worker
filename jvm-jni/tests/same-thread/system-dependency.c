static int constructors;

void system_dependency_note_load(void) {
  ++constructors;
}

int system_dependency_loads(void) {
  return constructors;
}

int system_dependency_value(void) {
  return SYSTEM_DEPENDENCY_VALUE;
}
