#pragma once
#include "defines.hpp"
#include "kernel/common.hpp"
#include <atomic>
namespace fs::vfs
{
class pseudo_t;

class inode
{
  private:
    flag_t info;

  protected:
    u64 file_size;
    u64 index;
    /// hard link count
    std::atomic_uint64_t link_count;
    std::atomic_uint64_t ref_count;
    super_block *su_block;
    timeclock::microsecond_t last_read_time;
    timeclock::microsecond_t last_write_time;
    timeclock::microsecond_t last_attr_change_time;
    timeclock::microsecond_t birth_time;
    user_id owner;
    group_id group;
    u64 permission;
    pseudo_t *pseudo_data;

  public:
    inode()
        : info(0)
        , file_size(0)
        , index(0)
        , link_count(0)
        , ref_count(0)
        , su_block(nullptr)
        , last_read_time(0)
        , last_write_time(0)
        , last_attr_change_time(0)
        , birth_time(0)
        , owner(0)
        , group(0)
        , permission(permission_flags::all_xwr)
        , pseudo_data(nullptr)
    {
    }
    virtual ~inode() = default;
    // create file in disk
    virtual void create(dentry *entry);

    virtual bool has_permission(flag_t pf, user_id uid, group_id gid);

    virtual void mkdir(dentry *entry);
    virtual void rmdir();

    virtual void rename(dentry *new_entry);
    virtual void link(dentry *old_entry, dentry *new_entry);
    virtual bool unlink(dentry *entry);
    virtual bool create_symbolink(dentry *entry, const char *target);
    virtual const char *symbolink() = 0;

    bool create_pseudo(dentry *entry, inode_type_t t, u64 size);

    virtual u64 hash();

    void set_type(inode_type_t t) { info = (info & 0xFFFFFFFFFFFFFFF0) | (u64)t; }
    inode_type_t get_type() const { return (inode_type_t)(info & 0xF); }

    u64 get_size() const { return file_size; }
    void set_size(u64 size) { file_size = size; }

    void set_super_block(super_block *block) { su_block = block; }
    super_block *get_super_block() const { return su_block; }

    void set_index(u64 index) { this->index = index; }
    u64 get_index() const { return index; }

    timeclock::microsecond_t get_last_read_time() const { return last_read_time; }
    timeclock::microsecond_t get_last_write_time() const { return last_write_time; }
    timeclock::microsecond_t get_last_attr_change_time() const { return last_attr_change_time; }
    timeclock::microsecond_t get_birth_time() const { return birth_time; }

    void set_last_read_time(timeclock::microsecond_t t) { last_read_time = t; }
    void set_last_write_time(timeclock::microsecond_t t) { last_write_time = t; }
    void set_last_attr_change_time(timeclock::microsecond_t t) { last_attr_change_time = t; }
    void set_birth_time(timeclock::microsecond_t t) { birth_time = t; }

    void update_last_read_time();
    void update_last_write_time();
    void update_last_attr_change_time();

    user_id get_owner() const { return owner; }
    group_id get_group() const { return group; }

    void set_owner(user_id uid) { this->owner = uid; }
    void set_group(group_id gid) { this->group = gid; };
    void set_owner_group(user_id uid, group_id gid)
    {
        this->owner = uid;
        this->group = gid;
    }

    void set_permission(u64 p) { permission = p; }

    u64 get_link_count() const { return link_count; }
    u64 get_ref_count() const { return ref_count; }

    /// Acquire one open file description reference. Returns true for the first reference.
    bool acquire_open_reference();
    /// Release one open file description reference. Returns true for the last reference.
    bool release_open_reference();

    pseudo_t *get_pseudo_data() const { return pseudo_data; }
    void set_pseudo_data(pseudo_t *f) { pseudo_data = f; }

    u64 get_permission() const { return permission; }
};
} // namespace fs::vfs
