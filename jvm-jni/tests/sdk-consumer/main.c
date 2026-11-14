#include <host_bridge.h>
#include <loader_backend.h>
#include <jni_vm_adapter.h>
#include <jvm_adapter.h>
#include <dlopen_backend.h>

int main(void) {
  const struct gw_loader_api_v1 *api =
      gw_loader_query(GW_LOADER_ABI_V1, sizeof(struct gw_loader_api_v1));
  struct gw_jvm_profile_v1 empty = {0};
  struct gw_loader_error error = {0};
  int rejected = gw_jvm_jni_set_profile(&empty, &error);
  if (gw_dlopen_scope_enter(0) || gw_dlopen_scope_leave(0)) {
    return 1;
  }
  return api && rejected == GW_ERROR ? gw_jvm_jni_register() : 1;
}
