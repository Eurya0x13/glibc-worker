#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static void *open_image(const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    fprintf(stderr, "%s: %s\n", path, dlerror());
  }

  assert(handle);
  return handle;
}

static void check_provider(const char *name, const char *path) {
  void *loaded = dlopen(name, RTLD_NOLOAD | RTLD_LAZY);
  assert(loaded);
  assert(open_image(path) == loaded);
}

static void check_cxx(const char *path, const char *conflict) {
  assert(dlopen("libstdc++.so.6", RTLD_NOLOAD | RTLD_LAZY) == NULL);
  if (conflict) {
    assert(!dlopen("./module-cxx.so", RTLD_NOW | RTLD_LOCAL));
    const char *error = dlerror();
    assert(error && strstr(error, "Guest dependency conflict") &&
           strstr(error, conflict));
    return;
  }

  void *module = open_image("./module-cxx.so");
  int (*call)(void) = (int (*)(void))dlsym(module, "guest_cxx_probe");
  assert(call && call() == 42);
  check_provider("libstdc++.so.6", path);
}

int main(int argc, char **argv) {
  assert(argc >= 5 && argc <= 7);
  assert(dlopen("libdl.so.2", RTLD_NOLOAD | RTLD_LAZY) == NULL);
  assert(dlopen("libgcc_s.so.1", RTLD_NOLOAD | RTLD_LAZY) == NULL);

  /* Resolve both dependencies from a cold DT_NEEDED lookup. */
  void *module = open_image("./module-link.so");
  assert(open_image("./module.so") == module);
  void *dependency = dlopen("libdl.so.2", RTLD_NOLOAD | RTLD_LAZY);
  assert(dependency);

  void *packaged = open_image(argv[1]);
  assert(packaged == dependency);
  assert(open_image("libdl.so.2") == packaged);
  assert(open_image(argv[2]) == packaged);
  assert(open_image(argv[3]) == packaged);
  check_provider("libgcc_s.so.1", argv[4]);
  if (argc >= 6) {
    check_cxx(argv[5], argc == 7 ? argv[6] : NULL);
  }

  assert(dlopen("libdl.so", RTLD_NOW) == NULL);
  assert(dlopen("./dangling.so", RTLD_NOW) == NULL);
  assert(dlopen("./cycle-a.so", RTLD_NOW) == NULL);
  return 0;
}
