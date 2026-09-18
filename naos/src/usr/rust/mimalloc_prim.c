/* NaOS primitive layer for the vendored mimalloc runtime. */

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"

#include <naos/abi.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

typedef struct na_time_clock
{
    int64_t tv_sec;
    int64_t tv_nsec;
} na_time_clock_t;

extern int _s_clock(int clock_index, na_time_clock_t *clock);
extern void _s_exit(int64_t ret) __attribute__((noreturn));
extern int _s_getrandom(void *buffer, uint64_t length, uint32_t flags);
extern na_status_t _na_memory_map(na_memory_map_frame_t *frame);
extern na_status_t _na_memory_unmap(na_memory_unmap_frame_t *frame);
extern void _s_log(const char *message);

int naos_mimalloc_errno;

void abort(void)
{
    _s_exit(-1);
    __builtin_unreachable();
}

static int naos_status_error(na_status_t status)
{
    if (status == NA_STATUS_OK)
        return 0;
    return status == NA_STATUS_INVALID_ARGUMENT ? EINVAL : ENOMEM;
}

static bool naos_page_aligned(const void *addr, size_t size)
{
    const size_t page_size = _mi_os_page_size();
    return page_size != 0 && ((uintptr_t)addr % page_size) == 0 && (size % page_size) == 0;
}

void _mi_prim_mem_init(mi_os_mem_config_t *config)
{
    config->page_size = 4096;
    config->large_page_size = 0;
    config->alloc_granularity = 4096;
    config->physical_memory_in_kib = 0;
    config->virtual_address_bits = 48;
    config->has_overcommit = false;
    config->has_partial_free = true;
    config->has_virtual_reserve = false;
}

int _mi_prim_free(void *addr, size_t size)
{
    if (addr == NULL || size == 0 || !naos_page_aligned(addr, size))
        return EINVAL;
    na_memory_unmap_frame_t frame = {
        .struct_size = sizeof(frame),
        .flags = 0,
        .address = (uint64_t)(uintptr_t)addr,
        .length = size,
        .reserved0 = 0,
        .reserved1 = 0,
    };
    return naos_status_error(_na_memory_unmap(&frame));
}

int _mi_prim_alloc(void *hint_addr, size_t size, size_t try_alignment, bool commit, bool allow_large,
                  bool *is_large, bool *is_zero, void **addr)
{
    MI_UNUSED(try_alignment);
    MI_UNUSED(commit);
    MI_UNUSED(allow_large);
    *is_large = false;
    *is_zero = false;
    *addr = NULL;

    if (size == 0 || size > NA_MEMORY_MAP_MAX_BYTES || !naos_page_aligned((void *)0, size))
        return EINVAL;

    na_memory_map_frame_t frame = {
        .struct_size = sizeof(frame),
        .flags = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE,
        /* NaOS anonymous mappings ignore allocator hints here.  Keeping this
         * zero also prevents an internal mimalloc hint from colliding with an
         * executable MemoryObject VMA. */
        .hint = 0,
        .object = NA_HANDLE_INVALID,
        .offset = 0,
        .length = size,
        .address = 0,
        .data_offset = 0,
        .reserved0 = 0,
        .reserved1 = 0,
    };
    const na_status_t status = _na_memory_map(&frame);
    if (status != NA_STATUS_OK || frame.address == 0)
        return naos_status_error(status);
    *is_zero = true;
    *addr = (void *)(uintptr_t)frame.address;
    return 0;
}

int _mi_prim_commit(void *addr, size_t size, bool *is_zero)
{
    if (addr == NULL || size == 0 || !naos_page_aligned(addr, size))
        return EINVAL;
    na_memory_map_frame_t frame = {
        .struct_size = sizeof(frame),
        .flags = NA_MEMORY_MAP_COMMIT,
        .hint = (uint64_t)(uintptr_t)addr,
        .object = NA_HANDLE_INVALID,
        .offset = 0,
        .length = size,
        .address = 0,
        .data_offset = 0,
        .reserved0 = 0,
        .reserved1 = 0,
    };
    const na_status_t status = _na_memory_map(&frame);
    if (status != NA_STATUS_OK || frame.address != (uint64_t)(uintptr_t)addr)
        return naos_status_error(status == NA_STATUS_OK ? NA_STATUS_INVALID_ARGUMENT : status);
    *is_zero = true;
    return 0;
}

int _mi_prim_decommit(void *addr, size_t size, bool *needs_recommit)
{
    if (addr == NULL || size == 0 || !naos_page_aligned(addr, size))
        return EINVAL;
    na_memory_unmap_frame_t frame = {
        .struct_size = sizeof(frame),
        .flags = NA_MEMORY_UNMAP_DECOMMIT,
        .address = (uint64_t)(uintptr_t)addr,
        .length = size,
        .reserved0 = 0,
        .reserved1 = 0,
    };
    const na_status_t status = _na_memory_unmap(&frame);
    *needs_recommit = status == NA_STATUS_OK;
    return naos_status_error(status);
}

int _mi_prim_reset(void *addr, size_t size)
{
    _mi_memzero(addr, size);
    return 0;
}

int _mi_prim_reuse(void *addr, size_t size)
{
    MI_UNUSED(addr);
    MI_UNUSED(size);
    return 0;
}

int _mi_prim_protect(void *addr, size_t size, bool protect)
{
    MI_UNUSED(addr);
    MI_UNUSED(size);
    MI_UNUSED(protect);
    return 0;
}

int _mi_prim_alloc_huge_os_pages(void *hint_addr, size_t size, int numa_node, bool *is_zero, void **addr)
{
    MI_UNUSED(hint_addr);
    MI_UNUSED(size);
    MI_UNUSED(numa_node);
    *is_zero = false;
    *addr = NULL;
    return ENOTSUP;
}

size_t _mi_prim_numa_node(void)
{
    return 0;
}

size_t _mi_prim_numa_node_count(void)
{
    return 1;
}

mi_msecs_t _mi_prim_clock_now(void)
{
    na_time_clock_t clock = {0, 0};
    if (_s_clock(1, &clock) != 0 || clock.tv_sec < 0 || clock.tv_nsec < 0)
        return 0;
    if ((uint64_t)clock.tv_sec > (uint64_t)INT64_MAX / 1000U)
        return INT64_MAX;
    return (mi_msecs_t)(clock.tv_sec * 1000 + clock.tv_nsec / 1000000);
}

void _mi_prim_process_info(mi_process_info_t *pinfo)
{
    _mi_memzero(pinfo, sizeof(*pinfo));
}

void _mi_prim_out_stderr(const char *message)
{
    if (message != NULL)
        _s_log(message);
}

int _mi_prim_getenv(const char *name, char *result, size_t result_size)
{
    MI_UNUSED(name);
    MI_UNUSED(result);
    MI_UNUSED(result_size);
    return 0;
}

bool _mi_prim_random_buf(void *buffer, size_t length)
{
    return length == 0 || _s_getrandom(buffer, length, 0) == 0;
}

void _mi_prim_thread_init_auto_done(void)
{
}

void _mi_prim_thread_done_auto_done(void)
{
}

void _mi_prim_thread_associate_default_heap(mi_heap_t *heap)
{
    MI_UNUSED(heap);
}

bool _mi_is_redirected(void)
{
    return false;
}

bool _mi_allocator_init(const char **message)
{
    if (message != NULL)
        *message = NULL;
    return true;
}

void _mi_allocator_done(void)
{
}
