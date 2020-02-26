#pragma once
#include "../../mm/list_node_cache.hpp"
#include "../../util/linked_list.hpp"
#include "common.hpp"
#include "defines.hpp"

namespace fs::vfs
{

class dentry
{
  public:
    using dentry_list_t = util::linked_list<dentry *>;
    using dentry_list_node_allocator_t = memory::list_node_cache_allocator<dentry_list_t>;

  protected:
    const char *name;
    inode *node;
    dentry *parent;
    bool loaded_child;
    bool mount_point;

    dentry_list_t child_list;

  public:
    dentry();
    virtual ~dentry() = default;

    virtual u64 hash() const;
    dentry *find_child(const char *name) const;
    void set_inode(inode *node);
    inode *get_inode() const;

    void set_parent(dentry *parent);
    dentry *get_parent() const;

    void set_name(const char *name);
    const char *get_name() const;

    void clean_mount_point() { mount_point = false; }
    void set_mount_point() { mount_point = true; }
    bool is_mount_point() { return mount_point; }

    bool is_load_child() const { return loaded_child; }
    void set_loaded(bool has_load) { loaded_child = has_load; }
    virtual void load_child();
    virtual void save_child();

    virtual void add_child(dentry *child);
    virtual void remove_child(dentry *child);
};

} // namespace fs::vfs
