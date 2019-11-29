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
    dentry *mount_entry;
    int count;
    mount_t(const char *mount_point, const char *dev, dentry *mount_entry, super_block *su_block)
        : mount_point(mount_point)
        , device_name(dev)
        , su_block(su_block)
        , mount_entry(mount_entry)
        , count(0){};
};
} // namespace fs::vfs
