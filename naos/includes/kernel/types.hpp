#pragma once
#include "freelibcxx/delegate.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/span.hpp"
#include "kernel/common.hpp"

#ifdef OS_KERNEL
namespace irq
{
enum class request_result
{
    ok = 0,
    no_handled = 1,
};

struct interrupt_info
{
    bool kernel_space;
    void *at;
    void *regs;
    uint32_t error_code;
};

using hard_handler = freelibcxx::delegate<request_result(const interrupt_info *, u64) noexcept>;
using soft_handler = freelibcxx::delegate<void(u64) noexcept>;

} // namespace irq
#endif

using thread_id = u64;
using process_id = u64;
using session_id = u64;
using group_id = u64;
using user_id = u64;
using dev_t = u64;

struct dirent
{
    const char *filename;
    u32 file_type;
    u32 file_name_len;
    u64 offset;
    u64 inode_index;
};
namespace timeclock
{

typedef u64 microsecond_t;
typedef u64 millisecond_t;
typedef u64 nanosecond_t;

} // namespace timeclock
