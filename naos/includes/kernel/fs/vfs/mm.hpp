#pragma once
#include "dentry.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/util/linked_list.hpp"
namespace fs::vfs
{
class file_system;

typedef util::linked_list<file_system *> fs_list_t;
typedef list_node_cache<fs_list_t> fs_list_node_cache_t;
typedef memory::ListNodeCacheAllocator<fs_list_node_cache_t> fs_list_node_allocator_t;

typedef list_node_cache<dentry::dentry_list_t> dentry_list_node_cache_t;
typedef memory::ListNodeCacheAllocator<dentry_list_node_cache_t> dentry_list_node_allocator_t;

extern fs_list_t *fs_list;
extern fs_list_node_cache_t *node_cache;
extern fs_list_node_allocator_t *allocator;

extern dentry_list_node_cache_t *dentry_list_node_cache;
extern dentry_list_node_allocator_t *dentry_list_node_allocator;
extern dentry *global_root;

} // namespace fs::vfs
