#pragma once

#include "naos/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers and consumes handle on success; the handle remains owned by the caller on failure. */
int naos_service_register_handle(const char *uri, na_handle_t handle);
/* Registers a duplicate of fd's native handle, preserving the fd. */
int naos_service_register_fd(const char *uri, int fd);
int naos_service_resolve(const char *uri, na_handle_t *handle);
int naos_service_unregister(const char *uri);
int naos_handle_close(na_handle_t handle);

#ifdef __cplusplus
}
#endif
