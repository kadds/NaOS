#pragma once
#include "common.hpp"

using handle_t = u64;
using thread_id = u64;
using process_id = u64;
using group_id = u64;
using file_desc = i32;
using user_id = u64;
using group_id = u64;
struct dirent
{
    const char *filename;
    u32 file_type;
    u32 file_name_len;
    u64 offset;
    u64 inode_index;
};
namespace time
{

struct time_t
{
    u16 microsecond; ///< 0 - 1000
    u16 millisecond; ///< 0 - 1000
    u8 second;       ///< 0 - 59
    u8 minute;       ///< 0 - 59
    u8 hour;         ///< 0 - 23
    u8 day;          ///< 0 - 30
    u8 month;        ///< 0 - 11
    u8 weak;         ///< 0 - 6
    u16 year;        ///< 2000 - ?
};

typedef u64 microsecond_t;
typedef u64 millisecond_t;
typedef u64 nanosecond_t;

} // namespace time
