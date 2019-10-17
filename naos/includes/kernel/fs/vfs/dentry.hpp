#pragma once
#include "common.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/util/linked_list.hpp"

namespace fs::vfs
{
class inode;
class dentry;

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
    bool mounted;

    dentry_list_t child_dir_list;
    dentry_list_t child_file_list;

  public:
    dentry();
    virtual ~dentry() = default;

    virtual u64 hash() const;
    dentry *find_child_dir(const char *name) const;
    dentry *find_child_file(const char *name) const;
    void set_inode(inode *node);
    inode *get_inode() const;

    void set_parent(dentry *parent);
    dentry *get_parent() const;

    void set_name(const char *name);
    const char *get_name() const;

    bool is_load_child() const { return loaded_child; }
    void set_loaded(bool has_load) { loaded_child = has_load; }
    virtual void load_child();
    virtual void save_child();

    virtual void add_sub_dir(dentry *child);
    virtual void add_sub_file(dentry *file);
    virtual void remove_sub_dir(dentry *child);
    virtual void remove_sub_file(dentry *file);
};

} // namespace fs::vfs
