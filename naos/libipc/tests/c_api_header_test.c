#include <naos/ipc_core.h>

int main(void)
{
    naos_ipc_resource_t resource = {0};
    return naos_ipc_resource_valid(&resource) == 0 ? 0 : 1;
}
