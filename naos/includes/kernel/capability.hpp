#pragma once

#include "freelibcxx/vector.hpp"
#include "handle.hpp"
#include "kobject.hpp"
#include "naos/abi.h"

namespace capability
{

enum class entry_state : u8
{
    reserved,
    active,
    // The source remains table-owned but cannot be looked up while a
    // restrict result is being written to user memory.
    restricting,
};

enum class location : u8
{
    table_root,
    in_transit,
};

struct metadata
{
    u32 binding = NA_BINDING_NONE;
    u32 reserved = 0;
    na_uuid_t protocol_uuid{};
    u64 scope = 0;
    u64 revision = 0;
    u64 features = 0;
    na_meta_rights_t meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT;
    u64 protocol_rights = 0;
};

inline constexpr na_meta_rights_t derive_tty_control_rights(na_meta_rights_t source)
{
    return source & (NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT);
}

struct entry
{
    khandle object;
    u64 generation = 0;
    entry_state state = entry_state::reserved;
    metadata meta{};
};

/// A resource owned by a queued message. Its capability hook remains active
/// until the resource is activated in a table or the message is discarded.
class transferred_resource
{
  public:
    transferred_resource() = default;

    transferred_resource(khandle object, metadata meta)
        : object_(std::move(object))
        , meta_(meta)
        , in_transit_(true)
    {
        acquire_transit();
    }

    transferred_resource(const transferred_resource &other)
        : object_(other.object_)
        , meta_(other.meta_)
        , in_transit_(other.in_transit_)
    {
        acquire_transit();
    }

    transferred_resource(transferred_resource &&other) noexcept
        : object_(std::move(other.object_))
        , meta_(other.meta_)
        , in_transit_(other.in_transit_)
    {
        other.in_transit_ = false;
    }

    transferred_resource &operator=(const transferred_resource &other)
    {
        if (this == &other)
            return *this;
        release_transit();
        object_ = other.object_;
        meta_ = other.meta_;
        in_transit_ = other.in_transit_;
        acquire_transit();
        return *this;
    }

    transferred_resource &operator=(transferred_resource &&other) noexcept
    {
        if (this == &other)
            return *this;
        release_transit();
        object_ = std::move(other.object_);
        meta_ = other.meta_;
        in_transit_ = other.in_transit_;
        other.in_transit_ = false;
        return *this;
    }

    ~transferred_resource() { release_transit(); }

    khandle &object() { return object_; }
    const khandle &object() const { return object_; }
    metadata &meta() { return meta_; }
    const metadata &meta() const { return meta_; }

    bool valid() const { return static_cast<bool>(object_); }
    bool in_transit() const { return in_transit_; }

    khandle take_object()
    {
        in_transit_ = false;
        if (object_)
            object_->on_capability_release(location::in_transit);
        return std::move(object_);
    }

    khandle take_object_to_table()
    {
        if (!object_)
            return {};

        // A MOVE replaces the source table root with the in-transit root.  The
        // destination must become observable before the transit root is
        // released, otherwise unique-object release hooks can run mid-move.
        object_->on_capability_acquire(location::table_root);
        in_transit_ = false;
        object_->on_capability_release(location::in_transit);
        return std::move(object_);
    }

    void release_transit()
    {
        if (!in_transit_)
            return;
        in_transit_ = false;
        if (object_)
            object_->on_capability_release(location::in_transit);
    }

  private:
    void acquire_transit()
    {
        if (in_transit_ && object_)
            object_->on_capability_acquire(location::in_transit);
    }

    khandle object_;
    metadata meta_{};
    bool in_transit_ = false;
};

struct transfer_record
{
    na_handle_t source = NA_HANDLE_INVALID;
    bool moved = false;
    transferred_resource resource;

    transfer_record(na_handle_t source, bool moved, transferred_resource resource)
        : source(source)
        , moved(moved)
        , resource(std::move(resource))
    {
    }
};

using transfer_record_list = freelibcxx::vector<transfer_record>;

} // namespace capability
