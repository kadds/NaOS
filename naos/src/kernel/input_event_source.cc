#include "kernel/input_event_source.hpp"

#include "kernel/ipc/channel.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"
#include "naos/canonical.hpp"
#include "naos/generated/system/InputEventSource.hpp"

namespace dev::input
{
namespace
{
input_event_source *global_input_event_source = nullptr;

void release_kernel_producer(khandle &producer)
{
    if (!producer)
        return;
    auto endpoint = producer.as<naos::ipc::raw_channel_endpoint>();
    auto *object = endpoint.operator&();
    if (object != nullptr && object->state() != nullptr)
        object->state()->kernel_owner_released(object->side());
    producer.reset();
}

} // namespace

input_event_source::input_event_source()
    : kobject(kobject::type_e::input_event_source)
{
}

input_event_source::~input_event_source()
{
    khandle producers[max_subscribers];
    khandle receivers[max_subscribers];
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 i = 0; i < max_subscribers; i++)
        {
            producers[i] = std::move(subscribers_[i].producer);
            receivers[i] = std::move(subscribers_[i].receiver);
            subscribers_[i].capacity = 0;
            subscribers_[i].overrun_pending = false;
            subscribers_[i].reserved = false;
        }
    }
    for (u64 i = 0; i < max_subscribers; i++)
    {
        release_kernel_producer(producers[i]);
        receivers[i].reset();
    }
}

bool input_event_source::subscribe(khandle &receiver, u64 max_events)
{
    receiver.reset();
    if (max_events == 0 || max_events > 64)
    {
        trace::debug("input: subscribe rejected invalid max_events=", max_events);
        return false;
    }
    subscriber *slot = nullptr;
    khandle stale_producer;
    khandle stale_receiver;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (auto &sub : subscribers_)
        {
            if (sub.reserved)
                continue;
            if (!sub.producer)
            {
                slot = &sub;
                break;
            }
            auto receiver_handle = sub.receiver.as<naos::ipc::raw_channel_endpoint>();
            auto *endpoint = receiver_handle.operator&();
            if (endpoint == nullptr || endpoint->state() == nullptr ||
                (endpoint->state()->signals(1 - endpoint->side()) & NA_SIGNAL_PEER_CLOSED) != 0)
            {
                stale_producer = std::move(sub.producer);
                stale_receiver = std::move(sub.receiver);
                sub.capacity = 0;
                sub.overrun_pending = false;
                slot = &sub;
                break;
            }
        }
        if (slot != nullptr)
            slot->reserved = true;
    }
    release_kernel_producer(stale_producer);
    stale_receiver.reset();
    if (slot == nullptr)
    {
        trace::debug("input: subscribe rejected active receiver still owns input");
        return false;
    }

    khandle producer;
    khandle receiver_object;
    na_channel_options_t options{};
    options.struct_size = sizeof(options);
    // Keep one message slot exclusively available for the latched overrun
    // marker.  A full event queue must not make loss permanently invisible.
    options.max_messages = max_events + 1;
    options.max_bytes = (max_events + 1) * 64;
    const auto channel_status = naos::ipc::create_raw_channel_kernel(producer, receiver_object, &options);
    if (channel_status != NA_STATUS_OK)
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            slot->reserved = false;
        }
        trace::debug("input: subscribe failed to create channel status=", static_cast<u64>(channel_status));
        return false;
    }

    auto producer_endpoint = producer.as<naos::ipc::raw_channel_endpoint>();
    auto *producer_object = producer_endpoint.operator&();
    if (producer_object == nullptr || producer_object->state() == nullptr)
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            slot->reserved = false;
        }
        receiver_object.reset();
        return false;
    }
    producer_object->state()->kernel_owner_acquired(producer_object->side());
    khandle published_receiver = receiver_object;
    bool committed = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!slot->reserved || slot->producer)
            slot->reserved = false;
        else
        {
            slot->producer = std::move(producer);
            slot->receiver = std::move(receiver_object);
            slot->capacity = max_events;
            slot->overrun_pending = false;
            slot->reserved = false;
            committed = true;
        }
    }
    if (!committed)
    {
        release_kernel_producer(producer);
        receiver_object.reset();
        published_receiver.reset();
        return false;
    }
    receiver = std::move(published_receiver);
    return true;
}

bool input_event_source::rollback_subscription(kobject *receiver)
{
    if (receiver == nullptr)
        return false;

    khandle producer;
    khandle receiver_object;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (auto &sub : subscribers_)
        {
            if (sub.receiver.operator&() != receiver)
                continue;
            producer = std::move(sub.producer);
            receiver_object = std::move(sub.receiver);
            sub.capacity = 0;
            sub.overrun_pending = false;
            break;
        }
    }
    if (!producer && !receiver_object)
        return false;
    release_kernel_producer(producer);
    receiver_object.reset();
    return true;
}

void input_event_source::publish(u64 key_code, u64 modifiers, u64 kind)
{
    std::uint8_t wire[128]{};
    std::uint8_t overrun_wire[128]{};

    naos::canonical::writer writer(wire, sizeof(wire));
    naos::system::InputEventSource::KeyEvent event{key_code, modifiers, kind};
    naos::system::InputEventSource::KeyEvent_encode(writer, event);
    if (!writer.good())
        return;

    naos::canonical::writer overrun_writer(overrun_wire, sizeof(overrun_wire));
    naos::system::InputEventSource::KeyEvent overrun_event{0, 0, NA_INPUT_EVENT_KIND_OVERRUN};
    naos::system::InputEventSource::KeyEvent_encode(overrun_writer, overrun_event);
    const bool overrun_ok = overrun_writer.good();

    struct publish_target
    {
        khandle producer;
        khandle receiver;
        u64 capacity = 0;
        bool overrun_pending = false;
    } targets[max_subscribers]{};
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 i = 0; i < max_subscribers; i++)
        {
            targets[i].producer = subscribers_[i].producer;
            targets[i].receiver = subscribers_[i].receiver;
            targets[i].capacity = subscribers_[i].capacity;
            targets[i].overrun_pending = subscribers_[i].overrun_pending;
        }
    }

    for (u64 i = 0; i < max_subscribers; i++)
    {
        auto &target = targets[i];
        if (!target.producer || !target.receiver)
            continue;
        auto receiver_endpoint = target.receiver.as<naos::ipc::raw_channel_endpoint>();
        auto *endpoint = receiver_endpoint.operator&();
        if (endpoint == nullptr || endpoint->state() == nullptr)
            continue;

        if (target.overrun_pending && overrun_ok)
        {
            const auto status = naos::ipc::send_raw_channel_kernel(
                target.producer, reinterpret_cast<const byte *>(overrun_wire), overrun_writer.size());
            if (status == NA_STATUS_OK)
                target.overrun_pending = false;
            else if (status == NA_STATUS_PEER_CLOSED)
                continue;
        }

        // Do not consume the reserved marker slot with another ordinary
        // event. The receiver will observe the marker after it drains.
        if (endpoint->state()->queued_messages(endpoint->side()) >= target.capacity)
        {
            // The extra channel slot was reserved specifically for this
            // marker. Publish it now: waiting for another physical input
            // event makes a final dropped event invisible forever.
            const auto marker_status =
                overrun_ok ? naos::ipc::send_raw_channel_kernel(
                                 target.producer, reinterpret_cast<const byte *>(overrun_wire), overrun_writer.size())
                           : NA_STATUS_RESOURCE_EXHAUSTED;
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            auto &sub = subscribers_[i];
            if (sub.producer.get_control() == target.producer.get_control())
            {
                sub.overrun_pending = marker_status != NA_STATUS_OK;
            }
            continue;
        }
        const auto status =
            naos::ipc::send_raw_channel_kernel(target.producer, reinterpret_cast<const byte *>(wire), writer.size());
        khandle stale_producer;
        khandle stale_receiver;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            auto &sub = subscribers_[i];
            if (sub.producer.get_control() == target.producer.get_control())
            {
                if (status == NA_STATUS_WOULD_BLOCK || status == NA_STATUS_RESOURCE_EXHAUSTED)
                {
                    sub.overrun_pending = true;
                }
                else if (status == NA_STATUS_PEER_CLOSED)
                {
                    // Move ownership out while protected, then release the
                    // endpoint and its kernel owner count after the lock.
                    stale_producer = std::move(sub.producer);
                    stale_receiver = std::move(sub.receiver);
                    sub.capacity = 0;
                    sub.overrun_pending = false;
                }
                else if (status == NA_STATUS_OK)
                    sub.overrun_pending = target.overrun_pending;
            }
        }
        release_kernel_producer(stale_producer);
        stale_receiver.reset();
    }
}

input_event_source *get_input_event_source() { return global_input_event_source; }

void publish_framebuffer_event(bool enabled)
{
    if (global_input_event_source != nullptr)
    {
        global_input_event_source->publish(
            0, 0, enabled ? NA_INPUT_EVENT_KIND_FRAMEBUFFER_ENABLE : NA_INPUT_EVENT_KIND_FRAMEBUFFER_DISABLE);
    }
}

khandle init_input_event_source()
{
    auto handle = handle_t<input_event_source>::make();
    auto *control = handle.get_control();
    // Keep one kernel-owned reference for the lifetime of the system. The
    // object pointer itself is published through global_input_event_source.
    control->ref++;
    global_input_event_source = handle.operator&();
    return handle;
}
} // namespace dev::input
