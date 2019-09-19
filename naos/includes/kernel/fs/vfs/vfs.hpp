#pragma once
#include "common.hpp"
namespace fs::vfs
{
class file_system;
class dentry;

void init();
int register_fs(file_system *fs);
int unregister_fs(file_system *fs);
file_system *get_file_system(const char *name);

bool mount(file_system *fs, const char *path);
bool umount(file_system *fs);

class file;

namespace mode
{
// open file mode
enum mode : flag_t
{
    read = 1,
    write = 2,
    bin = 4,
    append = 8,
};
} // namespace mode

// create file attribute,
namespace attribute
{
enum attribute : flag_t
{
    auto_create_file = 1,
    auto_create_dir_rescure = 2,
};
} // namespace attribute

dentry *path_walk(const char *name, dentry *root, flag_t create_attribute);

file *open(const char *path, flag_t mode, flag_t attr);

void close(file *file);
void mkdir(const char *dir, flag_t attr);
void rmdir(const char *dir);
void rename(const char *new_dir, const char *old_dir);
u64 write(file *f, byte *buffer, u64 size);
u64 read(file *f, byte *buffer, u64 max_size);
void flush(file *f);

u64 size(file *f);

} // namespace fs::vfs
