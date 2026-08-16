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

    // Transfer ownership without firing a capability-location hook.  Resource
    // tables use this while their map lock is held, then perform the table-root
    // and transit callbacks after unlocking.
    khandle take_object_without_callback()
    {
        in_transit_ = false;
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

class transfer_restore_token
{
  public:
    using discard_function = void (*)(void *, void *);

    transfer_restore_token() = default;

    transfer_restore_token(void *context, void *slot, discard_function discard)
        : context_(context)
        , slot_(slot)
        , discard_(discard)
    {
    }

    transfer_restore_token(const transfer_restore_token &) = delete;
    transfer_restore_token &operator=(const transfer_restore_token &) = delete;

    transfer_restore_token(transfer_restore_token &&other) noexcept
        : context_(other.context_)
        , slot_(other.slot_)
        , discard_(other.discard_)
        , callback_object_(std::move(other.callback_object_))
    {
        other.context_ = nullptr;
        other.slot_ = nullptr;
        other.discard_ = nullptr;
    }

    transfer_restore_token &operator=(transfer_restore_token &&other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        context_ = other.context_;
        slot_ = other.slot_;
        discard_ = other.discard_;
        callback_object_ = std::move(other.callback_object_);
        other.context_ = nullptr;
        other.slot_ = nullptr;
        other.discard_ = nullptr;
        return *this;
    }

    ~transfer_restore_token() { reset(); }

    bool valid() const { return slot_ != nullptr; }
    bool belongs_to(const void *context) const { return valid() && context_ == context; }
    void *slot() const { return slot_; }

    void set_callback_object(khandle object) { callback_object_ = std::move(object); }
    khandle &callback_object() { return callback_object_; }
    void disarm() { slot_ = nullptr; }

    void reset()
    {
        if (slot_ != nullptr && discard_ != nullptr)
            discard_(context_, slot_);
        context_ = nullptr;
        slot_ = nullptr;
        discard_ = nullptr;
        callback_object_.reset();
    }

  private:
    void *context_ = nullptr;
    void *slot_ = nullptr;
    discard_function discard_ = nullptr;
    khandle callback_object_;
};

struct transfer_record
{
    na_handle_t source = NA_HANDLE_INVALID;
    bool moved = false;
    transferred_resource resource;
    transfer_restore_token restore_token;

    transfer_record(na_handle_t source, bool moved, transferred_resource resource,
                    transfer_restore_token restore_token = {})
        : source(source)
        , moved(moved)
        , resource(std::move(resource))
        , restore_token(std::move(restore_token))
    {
    }
};

using transfer_record_list = freelibcxx::vector<transfer_record>;

} // namespace capability
