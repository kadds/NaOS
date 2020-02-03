#pragma once
#include "fs/vfs/defines.hpp"
#include "lock.hpp"
#include "mm/new.hpp"
#include "types.hpp"
#include "util/hash.hpp"
#include "util/hash_map.hpp"
#include "util/id_generator.hpp"
#include <atomic>
namespace task
{

using file_map_t = util::hash_map<file_desc, fs::vfs::file *>;

struct file_table_t
{
    std::atomic_int64_t count;
    lock::spinlock_t lock;
    file_map_t file_map;
    lock::rw_lock_t filemap_lock;
    util::id_level_generator<3> id_gen;
    fs::vfs::dentry *root, *current;
    file_table_t();
};

struct resource_table_t
{
  private:
    file_table_t *f_table;

  public:
    char **args;
    int argc;
    char *env;

  public:
    resource_table_t(file_table_t *ft = nullptr);
    ~resource_table_t();

    file_table_t *get_file_table() { return f_table; }
    /// TODO: implement this function
    void copy_file_table(file_table_t *raw_ft);

    file_desc new_file_desc(fs::vfs::file *);
    void delete_file_desc(file_desc fd);
    fs::vfs::file *get_file(file_desc fd);
    bool set_file(file_desc fd, fs::vfs::file *file);
};
} // namespace task
