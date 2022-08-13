#pragma once
#include "common.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/span.hpp"

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

typedef request_result (*request_func)(const interrupt_info *inter, u64 extra_data, u64 user_data);
typedef void (*soft_request_func)(u64 soft_irq_vector, u64 user_data);

} // namespace irq
#endif

using thread_id = u64;
using process_id = u64;
using group_id = u64;
using file_desc = i32;
using user_id = u64;
using group_id = u64;
using dev_t = u64;

constexpr file_desc console_in = 0;
constexpr file_desc console_out = 1;
constexpr file_desc console_err = 2;

inline constexpr file_desc invalid_file_desc = -1;

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

} // namespace time
