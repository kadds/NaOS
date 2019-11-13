#pragma once
#include "common.hpp"
#include "defines.hpp"
namespace fs::vfs
{
extern dentry *global_root;

void init();
int register_fs(file_system *fs);
int unregister_fs(file_system *fs);
file_system *get_file_system(const char *name);

bool mount(file_system *fs, const char *dev, const char *path);
bool umount(const char *path);

// create file attribute,

dentry *path_walk(const char *name, dentry *root, flag_t create_attribute);

file *open(const char *path, dentry *root, flag_t mode, flag_t attr);
void close(file *file);

bool mkdir(const char *dir, flag_t attr);
bool rmdir(const char *dir);

void rename(const char *new_dir, const char *old_dir);

u64 pathname(dentry *root, dentry *current, char *path, u64 max_len);

bool access(const char *pathname, flag_t mode);

u64 size(file *f);

} // namespace fs::vfs
