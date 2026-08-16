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
/* Registers a multi-client listener. listener must be a raw channel endpoint
 * whose peer stays with the provider; descriptor is a protocol descriptor. */
int naos_service_listen(const char *uri, na_handle_t listener, na_handle_t descriptor, uint64_t max_pending);
/* Connects to a registered listener and returns a fresh ClientEnd handle. */
int naos_service_connect(const char *uri, const na_uuid_t *expected_uuid, uint64_t requested_rights,
                         na_handle_t *client);
int naos_service_connect_versioned(const char *uri, const na_uuid_t *expected_uuid, uint64_t requested_rights,
                                   uint64_t requested_revision, uint64_t requested_features, na_handle_t *client);
int naos_service_unregister(const char *uri);
int naos_handle_close(na_handle_t handle);

#ifdef __cplusplus
}
#endif
