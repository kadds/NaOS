#pragma once
#include "../../mm/new.hpp"
#include "defines.hpp"
#include "freelibcxx/allocator.hpp"
namespace fs::vfs
{
struct nameidata
{
    dentry *last_available_entry;
    u16 symlink_deep;
    u64 deep;
    const char *last_scan_url_point;
    dir_path_str *entry_buffer;
    freelibcxx::Allocator *allocator_;

    nameidata(freelibcxx::Allocator *allocator)
        : entry_buffer(allocator->New<dir_path_str>())
        , allocator_(allocator)
    {
    }

    ~nameidata() { allocator_->Delete(entry_buffer); }
};
} // namespace fs::vfs
