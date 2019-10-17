#pragma once
#include "dentry.hpp"
#include "kernel/util/linked_list.hpp"
namespace fs::vfs
{
class file_system;

using file_system_list_t = util::linked_list<file_system *>;

extern file_system_list_t *fs_list;

extern dentry *global_root;

} // namespace fs::vfs
