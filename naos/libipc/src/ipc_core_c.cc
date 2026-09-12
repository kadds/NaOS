#include "ipc_core_c_common.inc"

// The channel fragment begins with the body of domain_create and ends by
// closing this C-linkage block. Keep the public entry-point declaration here
// so the generated fragments remain independently readable.
extern "C" {
naos_ipc_domain_t *naos_ipc_domain_create(const naos_ipc_domain_config_t *config)
#include "ipc_core_c_channel.inc"
#include "ipc_core_c_invocation.inc"
#include "ipc_core_c_ring.inc"
