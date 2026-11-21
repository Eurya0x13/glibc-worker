#include <host_bridge.h>
#include <loader_backend.h>
#include <jni_vm_adapter.h>
#include <jvm_adapter.h>
#include <signal_bootstrap.h>

int main(void) {
  if (gw_host_signal_bootstrap()) {
    return 1;
  }
  const struct gw_loader_api_v1 *api =
      gw_loader_query(GW_LOADER_ABI_V1, sizeof(struct gw_loader_api_v1));
  gw_jvm_profile_v2 empty = {};
  gw_loader_error error = {};
  int rejected = gw_jvm_jni_set_profile(&empty, &error);
  if (!api || rejected != GW_ERROR || gw_jvm_jni_register() != GW_OK) {
    return 1;
  }
  return gw_host_signal_restore();
}
