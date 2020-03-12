#pragma once
#include "common.hpp"
/// TODO: make the kernel object interface, growing vfs

enum class obj_type : u32
{
    file = 1,
    dentry,
    inode,
    spinlock,
    semaphore,
    rwlock,
    mutex,
    message_queue,
};

class kobject
{
    obj_type type;

  public:
    kobject(obj_type type)
        : type(type)
    {
    }

    obj_type get_type() { return type; }

    template <typename T, obj_type t> T *get()
    {
        if (likely(t == this->type))
            return (T *)this;
        return nullptr;
    }
};