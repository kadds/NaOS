#pragma once
#include "../../mm/new.hpp"
#include "defines.hpp"
namespace fs::vfs
{
struct nameidata
{
    dentry *last_available_entry;
    u16 symlink_deep;
    u64 deep;
    const char *last_scan_url_point;
    memory::MemoryView<dir_path_str> entry_buffer;

    nameidata(memory::IAllocator *allocator, u64 size, u64 align)
        : entry_buffer(allocator, size, align)
    {
    }
};
} // namespace fs::vfs
