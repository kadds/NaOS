#pragma once
#include "handle.hpp"
#include "kernel/common.hpp"
#include "naos/abi.h"

namespace capability
{
enum class location : u8;
}

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
        raw_channel_end,
        protocol_descriptor,
        protocol_client_end,
        protocol_server_end,
        invocation,
        responder,
        directory,
        memory_object,
        shared_ring,
        process,
    };

  public:
    kobject(type_e ty)
        : ty(ty)
    {
    }

    virtual ~kobject() {}

    type_e get_ktype() const { return ty; }

    virtual bool capability_is_unique() const { return false; }
    virtual void on_capability_acquire(capability::location) {}
    virtual void on_capability_release(capability::location) {}
    virtual void on_capability_handoff(capability::location, capability::location) {}
    virtual na_signal_t capability_signals() const { return 0; }
    virtual u64 capability_state() const { return 0; }

    template <typename T, type_e t> T *get_by()
    {
        if (likely(t == this->ty))
            return (T *)this;
        return nullptr;
    }
    template <typename T, type_e t> const T *get_by() const
    {
        if (likely(t == this->ty))
            return (const T *)this;
        return nullptr;
    }
    template <typename T> T *get()
    {
        if (likely(T::type_of() == this->ty))
            return (T *)this;
        return nullptr;
    }
    template <typename T> const T *get() const
    {
        if (likely(T::type_of() == this->ty))
            return (const T *)this;
        return nullptr;
    }

    template <typename T> bool is() const { return T::type_of() == this->ty; }

    template <typename T> T *get_unsafe() { return (T *)this; }
    template <typename T> const T *get_unsafe() const { return (const T *)this; }

  private:
    type_e ty;
};

using khandle = handle_t<kobject>;
