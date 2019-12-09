#pragma once
#include "fs/vfs/defines.hpp"
#include "lock.hpp"
#include "mm/new.hpp"
#include "types.hpp"
#include "util/hash_map.hpp"
#include "util/id_generator.hpp"
#include <atomic>
namespace task
{

struct hash_file_desc
{
    u64 operator()(const file_desc &fd) { return fd; }
};

using file_map_t = util::hash_map<file_desc, fs::vfs::file *, hash_file_desc>;

struct file_table
{
    std::atomic_int64_t count;
    lock::spinlock_t lock;
    file_map_t file_map;
    util::id_level_generator<3> id_gen;
    fs::vfs::dentry *root, *current;

    file_table();
    fs::vfs::dentry *get_path_root(const char *path);
};

struct resource_table_t
{
  private:
    file_table *f_table;

  public:
    char **args;
    int argc;
    char *env;

  public:
    resource_table_t(file_table *ft = nullptr);
    ~resource_table_t();

    void copy_file_table(file_table *raw_ft);
    file_table *get_file_table() { return f_table; }

    file_desc new_file_desc(fs::vfs::file *);
    void delete_file_desc(file_desc fd);
    fs::vfs::file *get_file(file_desc fd);
    void set_file(file_desc fd, fs::vfs::file *file);
};
} // namespace task
