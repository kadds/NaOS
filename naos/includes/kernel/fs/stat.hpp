#pragma once

#include "common.hpp"

// This is the x86-64 layout used by mlibc's struct stat. Keep this ABI
// independent from the libc headers so the kernel does not depend on mlibc.
struct naos_stat_timespec
{
    i64 tv_sec;
    i64 tv_nsec;
};

struct naos_stat
{
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    i32 st_uid;
    i32 st_gid;
    u32 __pad0;
    u64 st_rdev;
    i64 st_size;
    i64 st_blksize;
    i64 st_blocks;
    naos_stat_timespec st_atim;
    naos_stat_timespec st_mtim;
    naos_stat_timespec st_ctim;
    i64 __unused[3];
};

static_assert(sizeof(naos_stat) == 144, "NaOS stat ABI must match x86-64 mlibc");

namespace naos_stat_mode
{
inline constexpr u32 block = 0x06000;
inline constexpr u32 character = 0x02000;
inline constexpr u32 fifo = 0x01000;
inline constexpr u32 regular = 0x08000;
inline constexpr u32 directory = 0x04000;
inline constexpr u32 symlink = 0x0A000;
inline constexpr u32 socket = 0x0C000;

inline constexpr u32 user_read = 0400;
inline constexpr u32 user_write = 0200;
inline constexpr u32 user_execute = 0100;
inline constexpr u32 group_read = 0040;
inline constexpr u32 group_write = 0020;
inline constexpr u32 group_execute = 0010;
inline constexpr u32 other_read = 0004;
inline constexpr u32 other_write = 0002;
inline constexpr u32 other_execute = 0001;
} // namespace naos_stat_mode
