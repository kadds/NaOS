#ifndef NAOS_ABI_H
#define NAOS_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
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
#define NA_MEMORY_OBJECT_MAX_BYTES ((uint64_t)(16 << 20))
#define NA_MEMORY_MAP_MAX_BYTES ((uint64_t)(1ULL << 30))
#define NA_SHARED_RING_MAX_SLOTS ((uint64_t)256)
#define NA_SHARED_RING_MAX_SLOT_BYTES ((uint64_t)65536)
#define NA_SHARED_RING_MAX_BYTES ((uint64_t)(4 << 20))
#define NA_PROTOCOL_METHOD_BITMAP_WORDS 4
#define NA_PROTOCOL_MAX_METHOD_ID ((uint64_t)(NA_PROTOCOL_METHOD_BITMAP_WORDS * 64))

typedef enum na_status
{
    NA_STATUS_OK = 0,
    NA_STATUS_INVALID_HANDLE = 1,
    NA_STATUS_WRONG_BINDING = 2,
    NA_STATUS_WRONG_SCOPE = 3,
    NA_STATUS_ACCESS_DENIED = 4,
    NA_STATUS_INVALID_ARGUMENT = 5,
    NA_STATUS_INVALID_MESSAGE = 6,
    NA_STATUS_BUFFER_TOO_SMALL = 7,
    NA_STATUS_WOULD_BLOCK = 8,
    NA_STATUS_WAIT_TIMED_OUT = 9,
    NA_STATUS_RESOURCE_EXHAUSTED = 10,
    NA_STATUS_FAULT = 11,
    NA_STATUS_OBJECT_REVOKED = 12,
    NA_STATUS_PEER_CLOSED = 13,
    NA_STATUS_ALREADY_CONSUMED = 14,
    NA_STATUS_NOT_SUPPORTED = 15,
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
    NA_SIGNAL_COMPLETED = ((na_signal_t)1 << 4),
    NA_SIGNAL_CANCEL_REQUESTED = ((na_signal_t)1 << 5),
};

/* A minimal protocol-level call right.  Object-specific protocols may use
 * the remaining bits for finer-grained method authorization. */
enum
{
    NA_PROTOCOL_RIGHT_INVOKE = ((uint64_t)1 << 0),
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
    NA_BINDING_CLIENT_END = 2,
    NA_BINDING_SERVER_END = 3,
    NA_BINDING_KERNEL_VIEW = 4,
    NA_BINDING_INVOCATION = 5,
    NA_BINDING_RESPONDER = 6,
    NA_BINDING_MEMORY_OBJECT = 7,
    NA_BINDING_SHARED_RING = 8,
};

enum
{
    NA_PROCESS_RIGHT_WAIT = ((uint64_t)1 << 0),
    NA_PROCESS_RIGHT_INSPECT = ((uint64_t)1 << 1),
    NA_PROCESS_RIGHT_JOB_CONTROL = ((uint64_t)1 << 2),
};

enum
{
    NA_PROCESS_WAIT_FLAG_NOHANG = ((uint64_t)1 << 0),
};

typedef struct na_uuid
{
    uint8_t bytes[16];
} na_uuid_t;

typedef enum na_execution_outcome
{
    NA_EXECUTION_NONE = 0,
    NA_EXECUTION_NOT_DELIVERED = 1,
    NA_EXECUTION_OUTCOME_UNKNOWN = 2,
} na_execution_outcome_t;

typedef enum na_outcome_reason
{
    NA_OUTCOME_REASON_NONE = 0,
    NA_OUTCOME_REASON_PEER_CLOSED = 1,
    NA_OUTCOME_REASON_OBJECT_REVOKED = 2,
    NA_OUTCOME_REASON_OPERATION_DEADLINE = 3,
    NA_OUTCOME_REASON_CANCEL_REQUESTED = 4,
    NA_OUTCOME_REASON_REQUEST_DISCARDED = 5,
    NA_OUTCOME_REASON_RESPONDER_ABANDONED = 6,
    NA_OUTCOME_REASON_BROKER_FAILURE = 7,
    NA_OUTCOME_REASON_PROTOCOL_VIOLATION = 8,
} na_outcome_reason_t;

enum
{
    NA_CALL_FLAG_ONEWAY = ((uint32_t)1 << 0),
    NA_CALL_FLAG_FLEXIBLE = ((uint32_t)1 << 1),
};

/* Descriptor policy bits.  A one-way submit is admitted only when the
 * immutable descriptor explicitly opts the protocol into best-effort
 * notifications.  Individual generated methods may further restrict it. */
enum
{
    NA_PROTOCOL_FLAG_ALLOW_ONEWAY = ((uint32_t)1 << 0),
};

enum
{
    NA_MEMORY_FLAG_READ_ONLY = ((uint32_t)1 << 0),
    NA_MEMORY_FLAG_ZEROED = ((uint32_t)1 << 1),
};

enum
{
    NA_MEMORY_RIGHT_READ = ((uint64_t)1 << 0),
    NA_MEMORY_RIGHT_WRITE = ((uint64_t)1 << 1),
    NA_MEMORY_RIGHT_MAP = ((uint64_t)1 << 2),
    NA_MEMORY_RIGHT_INFO = ((uint64_t)1 << 3),
    NA_RING_RIGHT_PUSH = ((uint64_t)1 << 0),
    NA_RING_RIGHT_POP = ((uint64_t)1 << 1),
    NA_RING_RIGHT_INFO = ((uint64_t)1 << 2),
};

enum
{
    NA_RING_FLAG_NONBLOCK = ((uint32_t)1 << 0),
};

/* Canonical File/Stream request flags.  POSIX O_NONBLOCK is translated by
 * mlibc; it is never stored in native capability metadata. */
enum
{
    NA_IO_FLAG_NONBLOCK = ((uint64_t)1 << 0),
    NA_IO_FLAG_APPEND = ((uint64_t)1 << 1),
    NA_IO_FLAG_OVERRIDE = ((uint64_t)1 << 2),
};

enum
{
    NA_DIRECTORY_OPEN_FLAG_CHROOT = ((uint64_t)1 << 63),
};

typedef struct na_handle_restriction
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t scope;
    uint64_t revision;
    uint64_t features;
    na_meta_rights_t meta_rights;
    uint64_t protocol_rights;
} na_handle_restriction_t;

enum
{
    NA_RESTRICTION_SCOPE = ((uint32_t)1 << 0),
    NA_RESTRICTION_REVISION = ((uint32_t)1 << 1),
    NA_RESTRICTION_FEATURES = ((uint32_t)1 << 2),
    NA_RESTRICTION_META_RIGHTS = ((uint32_t)1 << 3),
    NA_RESTRICTION_PROTOCOL_RIGHTS = ((uint32_t)1 << 4),
};

typedef struct na_handle_info
{
    uint32_t struct_size;
    uint32_t binding;
    uint64_t scope;
    uint64_t revision;
    uint64_t features;
    na_meta_rights_t meta_rights;
    uint64_t protocol_rights;
    na_signal_t signals;
    uint64_t generation;
    uint64_t object_state;
    na_uuid_t protocol_uuid;
    uint64_t reserved0;
} na_handle_info_t;

typedef struct na_protocol_descriptor
{
    uint32_t struct_size;
    uint32_t flags;
    na_uuid_t uuid;
    uint64_t scope;
    uint64_t revision;
    uint64_t features;
    uint64_t protocol_rights;
    uint64_t method_count;
    uint64_t max_request_bytes;
    uint64_t max_response_bytes;
    uint64_t max_resources;
    uint64_t reserved0;
    uint64_t reserved1;
    uint64_t method_bitmap[NA_PROTOCOL_METHOD_BITMAP_WORDS];
    uint64_t oneway_bitmap[NA_PROTOCOL_METHOD_BITMAP_WORDS];
} na_protocol_descriptor_t;

typedef struct na_protocol_endpoint_options
{
    uint32_t struct_size;
    uint32_t flags;
    na_meta_rights_t client_meta_rights;
    na_meta_rights_t server_meta_rights;
    uint64_t max_messages;
    uint64_t max_bytes;
    uint64_t max_resources;
    uint64_t reserved0;
    uint64_t client_protocol_rights;
    uint64_t server_protocol_rights;
} na_protocol_endpoint_options_t;

typedef struct na_submit_frame
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t method_id;
    uint64_t request;
    uint64_t request_bytes;
    uint64_t resources;
    uint64_t resource_count;
    uint64_t operation_budget;
    uint64_t reserved0;
    uint64_t reserved1;
} na_submit_frame_t;

typedef struct na_result_frame
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t method_id;
    uint64_t bytes;
    uint64_t byte_capacity;
    uint64_t resources;
    uint64_t resource_capacity;
    uint64_t actual_bytes;
    uint64_t actual_resources;
    uint64_t required_bytes;
    uint64_t required_resources;
    uint32_t execution_outcome;
    uint32_t outcome_reason;
    int64_t protocol_error;
    uint64_t reserved0;
} na_result_frame_t;

typedef struct na_reply_frame
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t bytes;
    uint64_t byte_count;
    uint64_t resources;
    uint64_t resource_count;
    uint64_t reserved0;
    uint64_t reserved1;
} na_reply_frame_t;

typedef struct na_fail_frame
{
    uint32_t struct_size;
    uint32_t flags;
    uint32_t execution_outcome;
    uint32_t outcome_reason;
    uint64_t reserved0;
    uint64_t reserved1;
} na_fail_frame_t;

typedef struct na_bootstrap_frame
{
    uint32_t struct_size;
    uint32_t flags;
    na_handle_t root_directory;
    na_handle_t current_directory;
    na_handle_t service_directory;
    na_handle_t stdin_stream;
    na_handle_t stdout_stream;
    na_handle_t stderr_stream;
    uint64_t reserved0;
    uint64_t reserved1;
} na_bootstrap_frame_t;

#define NA_BOOTSTRAP_MESSAGE_VERSION ((uint32_t)1)
#define NA_BOOTSTRAP_RESOURCE_COUNT ((uint32_t)6)

enum
{
    NA_BOOTSTRAP_RESOURCE_ROOT_DIRECTORY = 0,
    NA_BOOTSTRAP_RESOURCE_CURRENT_DIRECTORY = 1,
    NA_BOOTSTRAP_RESOURCE_SERVICE_DIRECTORY = 2,
    NA_BOOTSTRAP_RESOURCE_STDIN = 3,
    NA_BOOTSTRAP_RESOURCE_STDOUT = 4,
    NA_BOOTSTRAP_RESOURCE_STDERR = 5,
};

/* The fixed part of a native child bootstrap message.  The first six
 * transferred resources are identified by the indices below; additional
 * resources may follow for an application-specific startup contract.
 * argc/envc are advisory startup metadata and are not used to authorize
 * capabilities. */
typedef struct na_bootstrap_message
{
    uint32_t struct_size;
    uint32_t flags;
    uint32_t version;
    uint32_t resource_count;
    uint32_t root_directory;
    uint32_t current_directory;
    uint32_t service_directory;
    uint32_t stdin_stream;
    uint32_t stdout_stream;
    uint32_t stderr_stream;
    uint32_t reserved0;
    uint32_t reserved1;
    uint64_t argc;
    uint64_t envc;
    uint64_t reserved2;
    uint64_t reserved3;
} na_bootstrap_message_t;

/* Replace the legacy path-based exec syscall.  The executable is acquired
 * through a Directory capability; the remaining fields are user pointers
 * consumed only while constructing the new process image. */
typedef struct na_process_exec_frame
{
    uint32_t struct_size;
    uint32_t flags;
    na_handle_t executable;
    uint64_t path;
    uint64_t argv;
    uint64_t envp;
    uint64_t reserved0;
    uint64_t reserved1;
} na_process_exec_frame_t;

/* Create a new process whose initial capability table is populated only by
 * the bootstrap channel endpoint and whose image is loaded from executable.
 * process and pid are output pointers; pid may be zero for native callers
 * that use the Process capability as their identity. */
typedef struct na_process_spawn_frame
{
    uint32_t struct_size;
    uint32_t flags;
    na_handle_t executable;
    na_handle_t bootstrap_endpoint;
    uint64_t path;
    uint64_t argv;
    uint64_t envp;
    uint64_t process;
    uint64_t pid;
    uint64_t reserved0;
    uint64_t reserved1;
} na_process_spawn_frame_t;

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
    uint64_t method_id;
    uint64_t bytes;
    uint64_t byte_capacity;
    uint64_t resources;
    uint64_t resource_capacity;
    na_handle_t responder;
    uint64_t actual_bytes;
    uint64_t actual_resources;
    uint64_t required_bytes;
    uint64_t required_resources;
    uint64_t reserved0;
} na_channel_receive_frame_t;

enum
{
    NA_MEMORY_MAP_READ = ((uint32_t)1 << 0),
    NA_MEMORY_MAP_WRITE = ((uint32_t)1 << 1),
    NA_MEMORY_MAP_EXEC = ((uint32_t)1 << 2),
    NA_MEMORY_MAP_SHARED = ((uint32_t)1 << 3),
};

typedef struct na_memory_map_frame
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t hint;
    na_handle_t object;
    uint64_t offset;
    uint64_t length;
    uint64_t address;
    uint64_t reserved0;
    uint64_t reserved1;
} na_memory_map_frame_t;

typedef struct na_memory_unmap_frame
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t address;
    uint64_t length;
    uint64_t reserved0;
    uint64_t reserved1;
} na_memory_unmap_frame_t;

typedef struct na_wait_item
{
    na_handle_t handle;
    na_signal_t signals;
    na_signal_t observed;
} na_wait_item_t;

/* Syscall numbers are compact v1 native ABI assignments. */
enum
{
    NA_SYSCALL_NONE = 0,
    NA_SYSCALL_LOG = 1,
    NA_SYSCALL_CLOCK_GET = 2,
    NA_SYSCALL_FUTEX = 3,
    NA_SYSCALL_EXIT = 4,
    NA_SYSCALL_EXIT_THREAD = 5,
    NA_SYSCALL_SLEEP = 6,
    NA_SYSCALL_CURRENT_PID = 7,
    NA_SYSCALL_CURRENT_TID = 8,
    NA_SYSCALL_SIGSEND = 9,
    NA_SYSCALL_SIGMASK = 10,
    NA_SYSCALL_SET_TCB = 11,
    NA_SYSCALL_FORK = 12,
    NA_SYSCALL_CLONE = 13,
    NA_SYSCALL_YIELD = 14,
    NA_SYSCALL_BRK = 15,
    NA_SYSCALL_SBRK = 16,
    NA_SYSCALL_HANDLE_CLOSE = 17,
    NA_SYSCALL_CHANNEL_CREATE = 18,
    NA_SYSCALL_CHANNEL_SEND = 19,
    NA_SYSCALL_CHANNEL_RECEIVE = 20,
    NA_SYSCALL_CHANNEL_DISCARD = 21,
    NA_SYSCALL_HANDLE_WAIT_MANY = 22,
    NA_SYSCALL_HANDLE_DUPLICATE = 23,
    NA_SYSCALL_HANDLE_RESTRICT = 24,
    NA_SYSCALL_HANDLE_GET_INFO = 25,
    NA_SYSCALL_PROTOCOL_DESCRIPTOR_CREATE = 26,
    NA_SYSCALL_PROTOCOL_ENDPOINT_CREATE = 27,
    NA_SYSCALL_INVOKE_SUBMIT = 28,
    NA_SYSCALL_INVOKE_SEND_ONEWAY = 29,
    NA_SYSCALL_INVOCATION_CANCEL = 30,
    NA_SYSCALL_INVOCATION_TAKE_RESULT = 31,
    NA_SYSCALL_RESPONDER_REPLY = 32,
    NA_SYSCALL_RESPONDER_FAIL = 33,
    NA_SYSCALL_BOOTSTRAP = 34,
    NA_SYSCALL_TTY_CONTROL_ACQUIRE = 35,
    NA_SYSCALL_MEMORY_MAP = 36,
    NA_SYSCALL_MEMORY_UNMAP = 37,
    NA_SYSCALL_PROCESS_EXEC = 38,
    NA_SYSCALL_PROCESS_HANDLE_OPEN = 39,
    NA_SYSCALL_PROCESS_SPAWN = 40,
    NA_SYSCALL_COUNT = 40,
};

#ifdef __cplusplus
}
#endif

#endif
