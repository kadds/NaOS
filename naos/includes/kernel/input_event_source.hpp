#pragma once

#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/resource.hpp"
#include "naos/abi.h"

namespace dev::input
{
class input_event_source : public kobject
{
  public:
    static constexpr kobject::type_e type_of() { return kobject::type_e::input_event_source; }

    input_event_source();
    ~input_event_source() override;

    bool capability_is_unique() const override { return false; }

    bool subscribe(khandle &receiver, u64 max_events);
    // Undo a subscription that has been prepared but whose response could
    // not be committed.  The receiver pointer is the reservation identity;
    // callers must not use it after the rollback.
    bool rollback_subscription(kobject *receiver);
    void publish(u64 key_code, u64 modifiers, u64 kind);

  private:
    // Hardware input has one active foreground owner. A replacement can take
    // over only after the current receiver has closed.
    static constexpr u64 max_subscribers = 1;

    struct subscriber
    {
        khandle producer;
        khandle receiver;
        u64 capacity = 0;
        bool overrun_pending = false;
        bool reserved = false;
    };

    subscriber subscribers_[max_subscribers]{};
    lock::spinlock_t lock_;
};

input_event_source *get_input_event_source();
khandle init_input_event_source();
void publish_framebuffer_event(bool enabled);
} // namespace dev::input
