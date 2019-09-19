#pragma once
namespace fs::vfs
{
class super_block;
class dentry;
struct mount_t
{
    const char *mount_point;
    const char *device_name;
    super_block *su_block;
    dentry *root;
    int count;
    mount_t(const char *mount_point, dentry *last_root)
        : mount_point(mount_point)
        , root(last_root){};
};
} // namespace fs::vfs
