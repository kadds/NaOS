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
#define NA_PROTOCOL_METHOD_BITMAP_WORDS 4
#define NA_PROTOCOL_MAX_METHOD_ID ((uint64_t)(NA_PROTOCOL_METHOD_BITMAP_WORDS * 64))

/* getrandom() flags. Hardware random sources are always non-blocking, so the
 * standard blocking/source flags do not change the hardware source. The
 * NaOS extension forces the use of RDSEED instead of the default fallback. */
#define NA_GETRANDOM_FLAG_NONBLOCK ((uint32_t)0x0001)
#define NA_GETRANDOM_FLAG_RANDOM ((uint32_t)0x0002)
#define NA_GETRANDOM_FLAG_INSECURE ((uint32_t)0x0004)
#define NA_GETRANDOM_FLAG_RDSEED ((uint32_t)0x0008)

/* Static TLS/TCB contract shared by native language runtimes. The kernel
 * stores the pointer in FS.base; it does not interpret the TLS image or the
 * runtime-specific extension after this prefix. */
#define NAOS_TLS_ABI_VERSION 1U
#define NAOS_TLS_MAX_SIZE ((uint64_t)(1ULL << 20))
#define NAOS_TLS_MAX_ALIGN ((uint64_t)(1ULL << 20))

typedef struct naos_tls_abi_v1
{
    void *self_pointer;
    uint64_t dtv_size;
    void **dtv_pointer;
    uint32_t tid;
    uint32_t did_exit;
    uint64_t reserved0;
    uint64_t stack_canary;
    uint32_t cancel_bits;
    uint32_t reserved1;
} naos_tls_abi_v1_t;

/* Native syscall transport results. These values are never negative; native
 * syscalls return one of these statuses and place successful results in their
 * frame or output parameters. */
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
    NA_STATUS_IO_ERROR = 16,
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

/* Capability epoll operations and readiness bits.  epoll_wait is a kernel
 * event queue operation; it does not expose a user-provided wait set. */
enum
{
    NA_EPOLL_CTL_ADD = 1,
    NA_EPOLL_CTL_MOD = 2,
    NA_EPOLL_CTL_DEL = 3,
};

enum
{
    NA_EPOLL_EVENT_READABLE = ((uint64_t)1 << 0),
    NA_EPOLL_EVENT_WRITABLE = ((uint64_t)1 << 1),
    NA_EPOLL_EVENT_ERROR = ((uint64_t)1 << 2),
    NA_EPOLL_EVENT_HANGUP = ((uint64_t)1 << 3),
    /* Registration flag, analogous to Linux EPOLLET. */
    NA_EPOLL_EVENT_EDGE_TRIGGERED = ((uint64_t)1 << 32),
};

/* InputEventSource kinds.  Keyboard press/release events use the first two
 * values; the remaining values are kernel-to-frontend control notifications
 * carried on the same single-owner input channel. */
enum
{
    NA_INPUT_EVENT_KIND_PRESS = 0,
    NA_INPUT_EVENT_KIND_RELEASE = 1,
    NA_INPUT_EVENT_KIND_OVERRUN = 2,
    NA_INPUT_EVENT_KIND_FRAMEBUFFER_DISABLE = 3,
    NA_INPUT_EVENT_KIND_FRAMEBUFFER_ENABLE = 4,
    NA_INPUT_EVENT_KIND_REPEAT = 5,
};

/* A minimal protocol-level call right.  Object-specific protocols may use
 * the remaining bits for finer-grained method authorization. */
enum
{
    NA_PROTOCOL_RIGHT_INVOKE = ((uint64_t)1 << 0),
    /* ServiceDirectory registry operations are deliberately separate from
     * public resolve/connect/list access. */
    NA_SERVICE_DIRECTORY_RIGHT_ADMIN = ((uint64_t)1 << 1),
    /* Permit registry mutations only below the system/service namespaces. */
    NA_SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER = ((uint64_t)1 << 3),
    /* Terminal protocol method rights.  They are checked by the invocation
     * layer against the generated per-method descriptor metadata. */
    NA_TERMINAL_RIGHT_READ = ((uint64_t)1 << 8),
    NA_TERMINAL_RIGHT_WRITE = ((uint64_t)1 << 9),
    NA_TERMINAL_RIGHT_CONTROL = ((uint64_t)1 << 10),
    NA_TERMINAL_RIGHT_WATCH = ((uint64_t)1 << 11),
    NA_TERMINAL_RIGHT_ADMIN = ((uint64_t)1 << 12),
    NA_DISPLAY_RIGHT_WRITER = ((uint64_t)1 << 3),
    /* VFS / block device stack named rights (bits 13..26, frozen by
     * doc/VFS_BLOCK_DEVICE_ADR.md §6) live in the IDL layer now: declared
     * per-protocol in idl/system + idl/internal and emitted as NA_*_RIGHT_*
     * macros by idl/naoidl.py into the generated protocol headers. */
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
    NA_BINDING_EPOLL = 8,
};

enum
{
    NA_PROCESS_RIGHT_WAIT = ((uint64_t)1 << 0),
    NA_PROCESS_RIGHT_INSPECT = ((uint64_t)1 << 1),
    NA_PROCESS_RIGHT_JOB_CONTROL = ((uint64_t)1 << 2),
    /* Start a child that was created with deferred execution. */
    NA_PROCESS_RIGHT_START = ((uint64_t)1 << 3),
};

enum
{
    /* The parent must explicitly start the returned Process capability. */
    NA_PROCESS_SPAWN_DEFERRED_START = ((uint32_t)1 << 0),
    /* Replace child stdout/stderr with the kernel klog stream on bootstrap. */
    NA_PROCESS_SPAWN_KLOG_STDIO = ((uint32_t)1 << 1),
};

enum
{
    NA_PROCESS_WAIT_FLAG_NOHANG = ((uint64_t)1 << 0),
    NA_PROCESS_WAIT_FLAG_UNTRACED = ((uint64_t)1 << 1),
};

static inline uint64_t na_process_wait_status_exit(int64_t exit_code)
{
    return ((uint64_t)exit_code & UINT64_C(0xff)) << 8;
}

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
    NA_OUTCOME_REASON_UNSUPPORTED = 9,
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
    /* Path-form queries only: skip the final symlink component
     * (POSIX lstat semantics); frozen by USERSPACE_FILESYSTEM_ADR §5.3.6. */
    NA_DIRECTORY_LOOKUP_FLAG_NOFOLLOW = ((uint64_t)1 << 0),
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
    /* For a MemoryObject, attenuate the source view by this relative range.
     * The resulting capability refers to the same storage identity and owns
     * no pages of its own. */
    uint64_t view_offset;
    uint64_t view_length;
} na_handle_restriction_t;

enum
{
    NA_RESTRICTION_SCOPE = ((uint32_t)1 << 0),
    NA_RESTRICTION_REVISION = ((uint32_t)1 << 1),
    NA_RESTRICTION_FEATURES = ((uint32_t)1 << 2),
    NA_RESTRICTION_META_RIGHTS = ((uint32_t)1 << 3),
    NA_RESTRICTION_PROTOCOL_RIGHTS = ((uint32_t)1 << 4),
    NA_RESTRICTION_RANGE = ((uint32_t)1 << 5),
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
    /* Stable opaque identity of the underlying capability object. The value
     * is not a handle and remains unchanged across MOVE/DUPLICATE transfers.
     * It must not encode a kernel address. */
    uint64_t object_id;
    /* Bounded view into the object identified by object_id. Offsets are
     * absolute within that storage identity; operations on the capability
     * use offsets relative to this view. */
    uint64_t view_offset;
    uint64_t view_length;
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
    uint64_t method_rights[NA_PROTOCOL_MAX_METHOD_ID];
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
    /* A responder may attach a negative POSIX errno to a typed failure. */
    int64_t protocol_error;
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
} na_bootstrap_frame_t;

/* A forked child may replace its terminal endpoint bindings before exec().
 * This mode updates the kernel-owned stdio capability references without
 * consuming the process bootstrap contract. */
#define NA_BOOTSTRAP_FLAG_REBIND_CONSOLE ((uint32_t)1u)

/* Early-service bootstrap omits the root/cwd resource slots and reuses
 * SERVICE_DIRECTORY plus STDIN/STDOUT/STDERR; the kernel-enforced minimum
 * resource count follows this branch. See USERSPACE_FILESYSTEM_ADR §5.3.5. */
#define NA_BOOTSTRAP_FLAG_EARLY_SERVICE ((uint32_t)2u)

#define NA_BOOTSTRAP_MESSAGE_VERSION ((uint32_t)5)
#define NA_BOOTSTRAP_RESOURCE_COUNT ((uint32_t)6)
#define NA_BOOTSTRAP_RESOURCE_NONE ((uint32_t)UINT32_MAX)
/* root, current directory, service directory and at least one stdio resource.
 * stdin/stdout/stderr may refer to that same terminal binding. */
#define NA_BOOTSTRAP_MIN_RESOURCE_COUNT ((uint32_t)4)
/* service directory and three stdio streams. Kernel-owned authorities are
 * acquired through ServiceDirectory rather than this startup contract. */
#define NA_BOOTSTRAP_EARLY_MIN_RESOURCE_COUNT ((uint32_t)4)

enum
{
    NA_BOOTSTRAP_RESOURCE_ROOT_DIRECTORY = 0,
    NA_BOOTSTRAP_RESOURCE_CURRENT_DIRECTORY = 1,
    NA_BOOTSTRAP_RESOURCE_SERVICE_DIRECTORY = 2,
    NA_BOOTSTRAP_RESOURCE_STDIN = 3,
    NA_BOOTSTRAP_RESOURCE_STDOUT = 4,
    NA_BOOTSTRAP_RESOURCE_STDERR = 5,
};

/* The child bootstrap message carries only the standard namespace/stdio
 * resource indices. The stdio fields may alias one transferred terminal
 * resource so dup-style sharing survives spawn. argc/envc are advisory
 * startup metadata. Kernel-owned authorities are discovered by URI. */
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
    uint64_t argc;
    uint64_t envc;
    uint64_t reserved0;
    uint64_t reserved1;
} na_bootstrap_message_t;

/* A kernel-authorized locator for the current process' controlling terminal.
 * The token is opaque to callers and is accepted only by the terminal
 * manager for the matching terminal identity. */
typedef struct na_terminal_locator
{
    uint64_t terminal_id;
    uint64_t generation;
    uint8_t token[16];
} na_terminal_locator_t;

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
    /* Namespace capabilities to re-bootstrap after replacing the image. */
    na_handle_t root_directory;
    na_handle_t current_directory;
    na_handle_t service_directory;
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
    /* Output-only identity of the process that submitted the invocation.
     * Receivers must provide zero on input; the kernel stamps this field when
     * delivering a protocol request. Raw channel receives keep it zero. */
    uint64_t caller_pid;
} na_channel_receive_frame_t;

enum
{
    NA_MEMORY_MAP_READ = ((uint32_t)1 << 0),
    NA_MEMORY_MAP_WRITE = ((uint32_t)1 << 1),
    NA_MEMORY_MAP_EXEC = ((uint32_t)1 << 2),
    NA_MEMORY_MAP_SHARED = ((uint32_t)1 << 3),
    /* Re-commit pages in an existing anonymous VMA at `hint`. */
    NA_MEMORY_MAP_COMMIT = ((uint32_t)1 << 4),
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
    /* Output: logical bytes begin at this offset within the page-rounded VMA. */
    uint64_t data_offset;
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

enum
{
    /* Release anonymous physical pages while retaining their VMA. */
    NA_MEMORY_UNMAP_DECOMMIT = ((uint32_t)1 << 0),
};

typedef struct na_epoll_event
{
    uint64_t events;
    uint64_t data;
} na_epoll_event_t;

/* Create a pair of native file capabilities for the POSIX pipe wrapper. */
typedef struct na_pipe_create_frame
{
    na_handle_t read_end;
    na_handle_t write_end;
} na_pipe_create_frame_t;

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
    NA_SYSCALL_EPOLL_CREATE = 22,
    NA_SYSCALL_EPOLL_CTL = 23,
    NA_SYSCALL_EPOLL_WAIT = 24,
    NA_SYSCALL_HANDLE_DUPLICATE = 25,
    NA_SYSCALL_HANDLE_RESTRICT = 26,
    NA_SYSCALL_HANDLE_GET_INFO = 27,
    NA_SYSCALL_PROTOCOL_DESCRIPTOR_CREATE = 28,
    NA_SYSCALL_PROTOCOL_ENDPOINT_CREATE = 29,
    NA_SYSCALL_INVOKE_SUBMIT = 30,
    NA_SYSCALL_INVOKE_SEND_ONEWAY = 31,
    NA_SYSCALL_INVOCATION_CANCEL = 32,
    NA_SYSCALL_INVOCATION_TAKE_RESULT = 33,
    NA_SYSCALL_RESPONDER_REPLY = 34,
    NA_SYSCALL_RESPONDER_FAIL = 35,
    NA_SYSCALL_BOOTSTRAP = 36,
    NA_SYSCALL_TTY_CONTROL_ACQUIRE = 37,
    NA_SYSCALL_MEMORY_MAP = 38,
    NA_SYSCALL_MEMORY_UNMAP = 39,
    NA_SYSCALL_PROCESS_EXEC = 40,
    NA_SYSCALL_PROCESS_HANDLE_OPEN = 41,
    NA_SYSCALL_PROCESS_SPAWN = 42,
    NA_SYSCALL_PIPE_CREATE = 43,
    /* Create a MemoryObject; executables travel as MEMORY_OBJECT handles
     * in exec/spawn frames (USERSPACE_FILESYSTEM_ADR §5.3.2/.3). */
    NA_SYSCALL_MEMORY_CREATE = 44,
    /* Fill a user buffer with hardware-generated random bytes. */
    NA_SYSCALL_GETRANDOM = 45,
    /* Power off the platform through ACPI S5. */
    NA_SYSCALL_POWER_OFF = 46,
    NA_SYSCALL_COUNT = 47,
};

#ifdef __cplusplus
}
#endif

#endif
