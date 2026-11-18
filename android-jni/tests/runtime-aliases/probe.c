#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>

static void *open_image(const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    fprintf(stderr, "%s: %s\n", path, dlerror());
  }

  assert(handle);
  return handle;
}

int main(int argc, char **argv) {
  assert(argc == 4);
  assert(dlopen("libdl.so.2", RTLD_NOLOAD | RTLD_LAZY) == NULL);

  /* The two links lead to a DSO with a cold DT_NEEDED on libdl.so.2. */
  void *module = open_image("./module-link.so");
  assert(open_image("./module.so") == module);
  void *dependency = dlopen("libdl.so.2", RTLD_NOLOAD | RTLD_LAZY);
  assert(dependency);

  void *packaged = open_image(argv[1]);
  assert(packaged == dependency);
  assert(open_image("libdl.so.2") == packaged);
  assert(open_image(argv[2]) == packaged);
  assert(open_image(argv[3]) == packaged);

  assert(dlopen("libdl.so", RTLD_NOW) == NULL);
  assert(dlopen("./dangling.so", RTLD_NOW) == NULL);
  assert(dlopen("./cycle-a.so", RTLD_NOW) == NULL);
  return 0;
}
