#ifndef NAOS_OBJECT_CALL_H
#define NAOS_OBJECT_CALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef uint64_t na_handle_t;

#define NA_HANDLE_INVALID ((na_handle_t)0)

/* The first native channel limits are deliberately conservative and public. */
#define NA_CHANNEL_MAX_MESSAGE_BYTES ((uint64_t)65536)
#define NA_CHANNEL_MAX_RESOURCES ((uint64_t)64)
#define NA_CHANNEL_MAX_MESSAGES ((uint64_t)1024)
#define NA_CHANNEL_DEFAULT_MAX_MESSAGES ((uint64_t)64)
#define NA_CHANNEL_DEFAULT_MAX_BYTES ((uint64_t)(1 << 20))
#define NA_CHANNEL_DEFAULT_MAX_RESOURCES ((uint64_t)256)
#define NA_CHANNEL_GLOBAL_MAX_MESSAGES ((uint64_t)4096)
#define NA_CHANNEL_GLOBAL_MAX_BYTES ((uint64_t)(16 << 20))
#define NA_CHANNEL_GLOBAL_MAX_RESOURCES ((uint64_t)16384)
#define NA_CAPABILITY_MAX_PER_PROCESS ((uint64_t)4096)

typedef enum na_status
{
    NA_STATUS_OK = 0,
    NA_STATUS_INVALID_HANDLE = 1,
    NA_STATUS_WRONG_BINDING = 2,
    NA_STATUS_ACCESS_DENIED = 3,
    NA_STATUS_INVALID_ARGUMENT = 4,
    NA_STATUS_INVALID_MESSAGE = 5,
    NA_STATUS_BUFFER_TOO_SMALL = 6,
    NA_STATUS_WOULD_BLOCK = 7,
    NA_STATUS_WAIT_TIMED_OUT = 8,
    NA_STATUS_RESOURCE_EXHAUSTED = 9,
    NA_STATUS_FAULT = 10,
    NA_STATUS_PEER_CLOSED = 11,
    NA_STATUS_ALREADY_CONSUMED = 12,
    NA_STATUS_OBJECT_REVOKED = 13,
    NA_STATUS_NOT_SUPPORTED = 14,
} na_status_t;

typedef uint64_t na_meta_rights_t;
enum
{
    NA_RIGHT_DUPLICATE = ((na_meta_rights_t)1 << 0),
    NA_RIGHT_TRANSFER = ((na_meta_rights_t)1 << 1),
    NA_RIGHT_WAIT = ((na_meta_rights_t)1 << 2),
    NA_RIGHT_INSPECT = ((na_meta_rights_t)1 << 3),
};

typedef uint64_t na_signal_t;
enum
{
    NA_SIGNAL_READABLE = ((na_signal_t)1 << 0),
    NA_SIGNAL_WRITABLE = ((na_signal_t)1 << 1),
    NA_SIGNAL_PEER_CLOSED = ((na_signal_t)1 << 2),
    NA_SIGNAL_OBJECT_REVOKED = ((na_signal_t)1 << 3),
};

enum
{
    NA_RESOURCE_MOVE = 1,
    NA_RESOURCE_DUPLICATE = 2,
};

enum
{
    NA_BINDING_NONE = 0,
    NA_BINDING_RAW_CHANNEL_END = 1,
};

typedef struct na_channel_options
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t max_messages;
    uint64_t max_bytes;
    uint64_t max_resources;
    uint64_t reserved0;
} na_channel_options_t;

typedef struct na_resource_disposition
{
    na_handle_t handle;
    uint32_t operation;
    uint32_t flags;
    na_meta_rights_t rights;
    uint64_t scope;
} na_resource_disposition_t;

typedef struct na_channel_send_frame
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t bytes;
    uint64_t byte_count;
    uint64_t resources;
    uint64_t resource_count;
    uint64_t reserved0;
    uint64_t reserved1;
} na_channel_send_frame_t;

typedef struct na_channel_receive_frame
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t bytes;
    uint64_t byte_capacity;
    uint64_t resources;
    uint64_t resource_capacity;
    uint64_t actual_bytes;
    uint64_t actual_resources;
    uint64_t required_bytes;
    uint64_t required_resources;
    uint64_t reserved0;
} na_channel_receive_frame_t;

typedef struct na_wait_item
{
    na_handle_t handle;
    na_signal_t signals;
    na_signal_t observed;
} na_wait_item_t;

/* These numbers are provisional v1 native ABI assignments. */
enum
{
    NA_SYSCALL_HANDLE_CLOSE = 73,
    NA_SYSCALL_CHANNEL_CREATE = 74,
    NA_SYSCALL_CHANNEL_SEND = 75,
    NA_SYSCALL_CHANNEL_RECEIVE = 76,
    NA_SYSCALL_CHANNEL_DISCARD = 77,
    NA_SYSCALL_HANDLE_WAIT_MANY = 78,
};

#ifdef __cplusplus
}
#endif

#endif
