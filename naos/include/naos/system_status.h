#pragma once

#include <stdint.h>

#include "naos/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NA_SYSTEM_STATUS_API_VERSION 1u
#define NA_PROCESS_STATUS_API_VERSION 1u
#define NA_PROCESS_COMMAND_LINE_API_VERSION 1u
#define NA_PROCESS_THREAD_LIST_API_VERSION 1u
#define NA_THREAD_STATUS_API_VERSION 1u
#define NA_SYSTEM_PROCESS_LIST_API_VERSION 1u
#define NA_SYSTEM_PROCESS_LIST_CAPACITY 64u

typedef struct na_system_status {
    uint32_t struct_size;
    uint32_t version;

    uint64_t sample_sequence;
    uint64_t sample_time_us;
    uint64_t page_size;

    uint64_t physical_pages;
    uint64_t usable_pages;
    uint64_t reserved_pages;
    uint64_t free_pages;
    uint64_t available_pages;

    uint64_t anonymous_pages;
    uint64_t file_cache_pages;
    uint64_t shared_pages;
    uint64_t reclaimable_pages;
    uint64_t kernel_reclaimable_pages;
    uint64_t kernel_unreclaimable_pages;
    uint64_t kernel_pages;
    uint64_t user_resident_pages;
    uint64_t user_committed_pages;

    uint64_t committed_pages;
    uint64_t commit_limit_pages;

    uint64_t swap_total_pages;
    uint64_t swap_free_pages;
    uint64_t swap_used_pages;

    uint64_t process_count;
    uint64_t thread_count;
    uint64_t vma_count;
    uint64_t page_faults;
    uint64_t allocation_failures;
    uint64_t flags;
} na_system_status_t;

typedef struct na_process_status {
    uint32_t struct_size;
    uint32_t version;
    uint64_t sample_sequence;
    uint64_t sample_time_us;
    uint64_t page_size;
    uint64_t pid;
    uint64_t parent_pid;
    uint64_t thread_count;
    uint64_t live_thread_count;
    uint64_t vma_count;
    uint64_t virtual_pages;
    uint64_t mapped_pages;
    uint64_t rss_pages;
    uint64_t private_pages;
    uint64_t shared_pages;
    uint64_t committed_pages;
    uint64_t anonymous_pages;
    uint64_t file_cache_pages;
    uint64_t kernel_stack_pages;
    uint64_t user_stack_pages;
    uint64_t page_faults;
    uint64_t peak_rss_pages;
    uint64_t user_time_us;
    uint64_t system_time_us;
    /* Points into the caller-supplied, mapped name buffer. */
    const char *name;
    uint64_t name_bytes;
    uint64_t name_required_bytes;
} na_process_status_t;

typedef struct na_thread_status {
    uint32_t struct_size;
    uint32_t version;
    uint64_t tid;
    uint64_t state;
    uint64_t attributes;
    uint64_t cpu_id;
    uint64_t static_priority;
    int64_t dynamic_priority;
    uint64_t user_time_us;
    uint64_t system_time_us;
    /* Points into the caller-supplied, mapped name buffer. */
    const char *name;
    uint64_t name_bytes;
} na_thread_status_t;

typedef struct na_process_thread_list {
    uint32_t struct_size;
    uint32_t version;
    uint64_t after_tid;
    uint64_t next_after_tid;
    uint64_t name_bytes;
    uint64_t name_required_bytes;
    uint32_t count;
    uint32_t reserved0;
    na_thread_status_t entries[NA_SYSTEM_PROCESS_LIST_CAPACITY];
} na_process_thread_list_t;

typedef struct na_system_process_entry {
    uint64_t pid;
    na_handle_t handle;
} na_system_process_entry_t;

typedef struct na_system_process_list {
    uint32_t struct_size;
    uint32_t version;
    uint64_t after_pid;
    uint64_t next_after_pid;
    uint32_t count;
    uint32_t reserved0;
    na_system_process_entry_t entries[NA_SYSTEM_PROCESS_LIST_CAPACITY];
} na_system_process_list_t;

int naos_system_status_open(na_handle_t *handle);
int naos_system_status_get(na_handle_t handle, na_system_status_t *status);
int naos_system_status_list_processes(na_handle_t handle, na_system_process_list_t *list);
void naos_system_process_list_close(na_system_process_list_t *list);
int naos_process_status_get(na_handle_t handle, na_process_status_t *status, na_handle_t name_buffer,
                            void *name_address, uint64_t name_buffer_size);
/* Copy the NUL-terminated argv command line into the caller-owned,
 * writable MemoryObject. The object must be mapped by the caller. The
 * required size is returned even when buffer_size is too small. */
int naos_process_command_line_get(na_handle_t process, na_handle_t buffer, uint64_t buffer_size,
                                  uint64_t *actual_bytes, uint64_t *required_bytes);
int naos_process_thread_list_get(na_handle_t process, na_handle_t name_buffer, void *name_address,
                                 uint64_t name_buffer_size, na_process_thread_list_t *list);

#ifdef __cplusplus
}
#endif
