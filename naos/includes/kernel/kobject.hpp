#pragma once
#include "common.hpp"
#include "handle.hpp"

class kobject
{
  public:
    enum class type_e : u32
    {
        unknown = 0,
        file = 1,
        dentry,
        inode,
        spinlock,
        semaphore,
        rwlock,
        mutex,
        message_queue,
        list_entries,
    };

  public:
    kobject(type_e ty)
        : ty(ty)
    {
    }

    virtual ~kobject() {}

    type_e get_ktype() { return ty; }

    template <typename T, type_e t> T *get_by()
    {
        if (likely(t == this->ty))
            return (T *)this;
        return nullptr;
    }
    template <typename T> T *get()
    {
        if (likely(T::type_of() == this->ty))
            return (T *)this;
        return nullptr;
    }

    template <typename T> bool is() { return T::type_of() == this->ty; }

    template <typename T> T *get_unsafe() { return (T *)this; }

  private:
    type_e ty;
};

using khandle = handle_t<kobject>;
