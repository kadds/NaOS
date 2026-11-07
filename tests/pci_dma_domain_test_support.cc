#include "freelibcxx/extern.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/kobject.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/new.hpp"
#include <cstdlib>

namespace memory
{
KernelCommonAllocator allocator;
KernelCommonAllocator *KernelCommonAllocatorV = &allocator;

void *KernelCommonAllocator::allocate(u64 size, u64 alignment) noexcept
{
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    void *allocation = nullptr;
    if (::posix_memalign(&allocation, static_cast<size_t>(alignment), static_cast<size_t>(size)) != 0)
        return nullptr;
    return allocation;
}

void KernelCommonAllocator::deallocate(void *allocation) noexcept { ::free(allocation); }
} // namespace memory

namespace log
{
bool enabled(level, module) { return false; }

namespace detail
{
void emit_message(level, module, const char *, u32, bool, const char *, u64) {}
void emit_panic_message(module, const char *, u32, bool, const char *, u64, const regs_t *) {}
} // namespace detail

[[noreturn]] void keep_panic(const regs_t *) { std::abort(); }
} // namespace log

namespace arch::idt
{
bool save_and_disable() { return false; }
void enable() {}
void disable() {}
} // namespace arch::idt

void get_task_id(u64 &pid, u64 &tid)
{
    pid = 0;
    tid = 0;
}

void kobject::notify_readiness() {}

kobject::kobject(type_e type)
    : ty(type)
    , object_id_(kobject_detail::next_object_id.fetch_add(1, std::memory_order_relaxed))
    , readiness_watchers_(nullptr)
{
}

namespace naos::data_plane
{
memory_object::memory_object(u64 size, u32 flags)
    : kobject(type_of())
    , bytes_(nullptr)
    , flags_(flags)
    , pages_(nullptr)
    , pages_size_(size)
    , pages_owned_(false)
    , page_states_(nullptr)
{
}

memory_object::~memory_object() = default;

na_status_t memory_object::dma_page_frame(u64 object_offset, dma_direction direction, phy_addr_t &frame) const
{
    constexpr u64 test_page_size = 4096;
    frame = nullptr;
    if ((object_offset & (test_page_size - 1)) != 0 || object_offset > pages_size_ ||
        test_page_size > pages_size_ - object_offset)
        return NA_STATUS_INVALID_ARGUMENT;
    if ((flags_ & NA_MEMORY_FLAG_READ_ONLY) != 0 && direction != dma_direction::device_reads)
        return NA_STATUS_ACCESS_DENIED;
    frame = phy_addr_t::from(0x9000'0000ull + object_offset);
    return NA_STATUS_OK;
}
} // namespace naos::data_plane
