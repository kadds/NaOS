#pragma once
#include "freelibcxx/hash.hpp"
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "fs/vfs/defines.hpp"
#include "handle.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/kobject.hpp"
#include "lock.hpp"
#include "mm/new.hpp"
#include "types.hpp"
#include "util/id_generator.hpp"
#include <atomic>
namespace task
{

using handle_map_t = freelibcxx::hash_map<file_desc, khandle>;

class list_dir_functor
{
  public:
  private:
    freelibcxx::vector<handle_t<fs::vfs::dentry>> dirs;
};

class resource_table_t
{
  private:
    handle_map_t handle_map;
    lock::rw_lock_t map_lock;

    util::id_level_generator<3> id_gen;
    fs::vfs::dentry *process_root, *process_current;

  public:
    resource_table_t();
    ~resource_table_t();
    using filter_func = bool(file_desc desc, khandle handle, u64 userdata);

    void clone(resource_table_t *to, filter_func filter, u64 userdata);

    file_desc new_kobject(khandle obj);

    void delete_kobject(file_desc fd);

    khandle get_kobject(file_desc fd);

    bool set_handle(file_desc fd, khandle obj);

    void clear();

    fs::vfs::dentry *root() const { return process_root; }
    fs::vfs::dentry *current() const { return process_current; }

    void set_root(fs::vfs::dentry *root) { process_root = root; }
    void set_current(fs::vfs::dentry *current) { process_current = current; }
};
} // namespace task
