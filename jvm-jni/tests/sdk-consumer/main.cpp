#include <host_bridge.h>
#include <loader_backend.h>
#include <jni_vm_adapter.h>
#include <jvm_adapter.h>
int main(void) {
  const struct gw_loader_api_v1 *api = gw_loader_query(GW_LOADER_ABI_V1, sizeof(struct gw_loader_api_v1));
  gw_jvm_profile_v2 empty = {};
  gw_loader_error error = {};
  int rejected = gw_jvm_jni_set_profile(&empty, &error);
  return api && rejected == GW_ERROR ? gw_jvm_jni_register() : 1;
}
