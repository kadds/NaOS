#pragma once
#include "common.hpp"

#ifdef OS_KERNEL
namespace irq
{
enum class request_result
{
    ok = 0,
    no_handled = 1,
};

typedef request_result (*request_func)(const void *regs, u64 extra_data, u64 user_data);
typedef void (*soft_request_func)(u64 soft_irq_vector, u64 user_data);

} // namespace irq
#endif

using handle_t = u64;
using thread_id = u64;
using process_id = u64;
using group_id = u64;
using file_desc = i32;
using user_id = u64;
using group_id = u64;
using dev_t = u64;

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

struct time_t
{
    u16 microsecond; ///< 0 - 1000
    u16 millisecond; ///< 0 - 1000
    u8 second;       ///< 0 - 59
    u8 minute;       ///< 0 - 59
    u8 hour;         ///< 0 - 23
    u8 mday;         ///< 0 - 30
    u8 month;        ///< 0 - 11
    u8 wday;         ///< 0 - ?
    u16 yday;        ///< 0 - 364(365)
    u16 year;        ///< 2000 - ?
};

typedef u64 microsecond_t;
typedef u64 millisecond_t;
typedef u64 nanosecond_t;

} // namespace time
