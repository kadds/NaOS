#pragma once

#include "naos/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

int naos_service_register_fd(const char *name, int fd);
int naos_service_resolve(const char *name, na_handle_t *handle);
int naos_service_unregister(const char *name);
int naos_handle_close(na_handle_t handle);

#ifdef __cplusplus
}
#endif
