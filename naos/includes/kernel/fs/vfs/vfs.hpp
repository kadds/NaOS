#pragma once
#include "common.hpp"
#include "defines.hpp"
namespace fs::vfs
{
void init();
int register_fs(file_system *fs);
int unregister_fs(file_system *fs);
file_system *get_file_system(const char *name);

bool mount(file_system *fs, const char *path);
bool umount(file_system *fs);

// create file attribute,

dentry *path_walk(const char *name, dentry *root, flag_t create_attribute);

file *open(const char *path, flag_t mode, flag_t attr);
void close(file *file);

void mkdir(const char *dir, flag_t attr);
void rmdir(const char *dir);

void rename(const char *new_dir, const char *old_dir);

u64 size(file *f);

} // namespace fs::vfs
