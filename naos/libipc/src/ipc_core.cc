#include <naos/ipc_core.hpp>

#include <atomic>
#include <cstring>
#include <new>
#include <utility>

namespace naos::ipc_core
{
namespace
{
class lock_guard
{
  public:
    explicit lock_guard(lock &value) noexcept
        : value_(value)
    {
        value_.acquire();
    }

    ~lock_guard() { value_.release(); }

    lock_guard(const lock_guard &) = delete;
    lock_guard &operator=(const lock_guard &) = delete;

  private:
    lock &value_;
};

bool valid_allocator(const allocator *value) noexcept { return value != nullptr; }
bool valid_lock(const lock *value) noexcept { return value != nullptr; }

void notify(wait_notifier *notifier) noexcept
{
    if (notifier != nullptr)
        notifier->notify();
}
} // namespace

domain::domain(const domain_config &config) noexcept
    : allocator_(*config.memory)
    , lock_(*config.synchronization)
    , max_messages_(config.max_messages)
    , max_bytes_(config.max_bytes)
    , max_resources_(config.max_resources)
    , valid_(true)
{
}

domain *domain::create(const domain_config &config) noexcept
{
    if (!valid_allocator(config.memory) || !valid_lock(config.synchronization) || config.max_messages == 0 ||
        config.max_bytes == 0 || config.max_resources == 0)
        return nullptr;
    void *storage = config.memory->allocate(sizeof(domain), alignof(domain));
    if (storage == nullptr)
        return nullptr;
    return ::new (storage) domain(config);
}

void domain::destroy() noexcept
{
    allocator &memory = allocator_;
    this->~domain();
    memory.deallocate(this, sizeof(domain), alignof(domain));
}

bool domain::reserve(std::uint64_t bytes, std::uint64_t resources) noexcept
{
    lock_guard guard(lock_);
    if (messages_ >= max_messages_ || bytes > max_bytes_ - bytes_ || resources > max_resources_ - resources_)
        return false;
    messages_++;
    bytes_ += bytes;
    resources_ += resources;
    return true;
}

void domain::release(std::uint64_t bytes, std::uint64_t resources) noexcept
{
    lock_guard guard(lock_);
    messages_--;
    bytes_ -= bytes;
    resources_ -= resources;
}

message::message(channel *owner, allocator &control_allocator, allocator &payload_allocator, std::uint64_t byte_count,
                 std::uint64_t resource_capacity) noexcept
    : owner_(owner)
    , control_allocator_(control_allocator)
    , payload_allocator_(payload_allocator)
    , byte_count_(byte_count)
    , resource_capacity_(resource_capacity)
{
}

message *message::create(channel &channel, std::uint64_t byte_count, std::uint64_t resource_capacity) noexcept
{
    if (!channel.valid() || byte_count > channel.config_.max_bytes || resource_capacity > channel.config_.max_resources)
        return nullptr;
    allocator &control_memory = *channel.config_.control_memory;
    allocator &payload_memory = *channel.config_.payload_memory;
    void *storage = control_memory.allocate(sizeof(message), alignof(message));
    if (storage == nullptr)
        return nullptr;
    auto *result = ::new (storage) message(&channel, control_memory, payload_memory, byte_count, resource_capacity);
    if (byte_count != 0)
    {
        result->bytes_ = static_cast<std::uint8_t *>(payload_memory.allocate(byte_count, alignof(std::uint8_t)));
        if (result->bytes_ == nullptr)
        {
            result->~message();
            control_memory.deallocate(result, sizeof(message), alignof(message));
            return nullptr;
        }
    }
    if (resource_capacity != 0)
    {
        result->resources_ =
            static_cast<resource *>(control_memory.allocate(sizeof(resource) * resource_capacity, alignof(resource)));
        if (result->resources_ == nullptr)
        {
            result->~message();
            control_memory.deallocate(result, sizeof(message), alignof(message));
            return nullptr;
        }
        for (std::uint64_t index = 0; index < resource_capacity; index++)
            ::new (result->resources_ + index) resource();
    }
    result->valid_ = true;
    return result;
}

message::~message()
{
    if (resources_ != nullptr)
    {
        for (std::uint64_t index = 0; index < resource_capacity_; index++)
            resources_[index].~resource();
        control_allocator_.deallocate(resources_, sizeof(resource) * resource_capacity_, alignof(resource));
    }
    if (bytes_ != nullptr)
        payload_allocator_.deallocate(bytes_, byte_count_, alignof(std::uint8_t));
}

void message::destroy() noexcept
{
    allocator &memory = control_allocator_;
    this->~message();
    memory.deallocate(this, sizeof(message), alignof(message));
}

const resource *message::resource_at(std::uint64_t index) const noexcept
{
    if (!valid_ || index >= resource_count_)
        return nullptr;
    return &resources_[index];
}

na_status_t message::take_resource(std::uint64_t index, resource &result) noexcept
{
    if (!valid_ || index >= resource_count_)
        return NA_STATUS_INVALID_ARGUMENT;
    if (!resources_[index].valid())
        return NA_STATUS_INVALID_HANDLE;
    result = std::move(resources_[index]);
    return NA_STATUS_OK;
}

na_status_t message::restore_resource(std::uint64_t index, resource &&value) noexcept
{
    if (!valid_ || index >= resource_count_ || resources_[index].valid() || !value.valid())
        return NA_STATUS_INVALID_ARGUMENT;
    resources_[index] = std::move(value);
    return NA_STATUS_OK;
}

struct channel::queue
{
    message **storage = nullptr;
    std::uint64_t capacity = 0;
    std::uint64_t head = 0;
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    std::uint64_t resources = 0;
    bool claimed = false;
};

channel::channel(const channel_config &config) noexcept
    : config_(config)
    , max_messages_(config.max_messages)
    , max_bytes_(config.max_bytes)
    , max_resources_(config.max_resources)
{
}

channel::~channel() = default;

channel *channel::create(const channel_config &config) noexcept
{
    if (config.owner_domain == nullptr || !config.owner_domain->valid() || !valid_allocator(config.control_memory) ||
        !valid_allocator(config.payload_memory) || !valid_lock(config.synchronization) || config.max_messages == 0 ||
        config.max_bytes == 0 || config.max_resources == 0 || config.max_messages > NA_CHANNEL_MAX_MESSAGES ||
        config.max_bytes > NA_CHANNEL_MAX_MESSAGE_BYTES * 16 || config.max_resources > NA_CHANNEL_DEFAULT_MAX_RESOURCES)
        return nullptr;
    auto *memory = config.control_memory;
    void *storage = memory->allocate(sizeof(channel), alignof(channel));
    if (storage == nullptr)
        return nullptr;
    auto *result = ::new (storage) channel(config);
    result->queues_ = static_cast<queue *>(memory->allocate(sizeof(queue) * 2, alignof(queue)));
    if (result->queues_ == nullptr)
    {
        result->~channel();
        memory->deallocate(result, sizeof(channel), alignof(channel));
        return nullptr;
    }
    for (std::uint8_t side = 0; side < 2; side++)
    {
        ::new (result->queues_ + side) queue();
        auto &queue = result->queues_[side];
        queue.capacity = config.max_messages;
        queue.storage =
            static_cast<message **>(memory->allocate(sizeof(message *) * queue.capacity, alignof(message *)));
        if (queue.storage == nullptr)
        {
            for (std::uint8_t previous = 0; previous <= side; previous++)
            {
                if (result->queues_[previous].storage != nullptr)
                    memory->deallocate(result->queues_[previous].storage,
                                       sizeof(message *) * result->queues_[previous].capacity, alignof(message *));
                result->queues_[previous].~queue();
            }
            memory->deallocate(result->queues_, sizeof(queue) * 2, alignof(queue));
            result->~channel();
            memory->deallocate(result, sizeof(channel), alignof(channel));
            return nullptr;
        }
        for (std::uint64_t index = 0; index < queue.capacity; index++)
            queue.storage[index] = nullptr;
    }
    result->valid_ = true;
    return result;
}

void channel::clear_queue(queue &queue) noexcept
{
    for (;;)
    {
        message *value = nullptr;
        {
            lock_guard guard(*config_.synchronization);
            if (queue.count == 0)
                break;
            value = queue.storage[queue.head];
            queue.storage[queue.head] = nullptr;
            queue.head = (queue.head + 1) % queue.capacity;
            queue.count--;
            if (value != nullptr)
            {
                queue.bytes -= value->byte_count();
                queue.resources -= value->resource_count();
                release_global(value->byte_count(), value->resource_count());
            }
        }
        if (value != nullptr)
            value->destroy();
    }
    lock_guard guard(*config_.synchronization);
    queue.bytes = 0;
    queue.resources = 0;
    queue.claimed = false;
}

void channel::destroy() noexcept
{
    allocator &memory = *config_.control_memory;
    {
        lock_guard guard(*config_.synchronization);
        valid_ = false;
    }
    clear_queue(queues_[0]);
    clear_queue(queues_[1]);
    for (std::uint8_t side = 0; side < 2; side++)
    {
        memory.deallocate(queues_[side].storage, sizeof(message *) * queues_[side].capacity, alignof(message *));
        queues_[side].~queue();
    }
    memory.deallocate(queues_, sizeof(queue) * 2, alignof(queue));
    this->~channel();
    memory.deallocate(this, sizeof(channel), alignof(channel));
}

std::uint64_t channel::queued_messages(std::uint8_t side) const noexcept
{
    if (!valid_ || side > 1)
        return 0;
    lock_guard guard(*config_.synchronization);
    return queues_[side].count;
}

std::uint64_t channel::signals(std::uint8_t side) const noexcept
{
    if (!valid_ || side > 1)
        return 0;
    lock_guard guard(*config_.synchronization);
    const auto &receive = queues_[side];
    const auto &send = queues_[1 - side];
    std::uint64_t result = receive.count == 0 ? 0 : NA_SIGNAL_READABLE;
    if (side_references_[1 - side] != 0 && send.count < send.capacity && send.bytes < max_bytes_ &&
        send.resources < max_resources_)
        result |= NA_SIGNAL_WRITABLE;
    if (side_references_[1 - side] == 0)
        result |= NA_SIGNAL_PEER_CLOSED;
    return result;
}

bool channel::can_reap() const noexcept
{
    if (!valid_)
        return false;
    lock_guard guard(*config_.synchronization);
    return active_claims_ == 0 && active_operations_ == 0;
}

void channel::side_reference_acquired(std::uint8_t side) noexcept
{
    if (!valid_ || side > 1)
        return;
    lock_guard guard(*config_.synchronization);
    side_references_[side]++;
}

void channel::side_reference_released(std::uint8_t side) noexcept
{
    if (!valid_ || side > 1)
        return;
    {
        lock_guard guard(*config_.synchronization);
        if (side_references_[side] != 0)
            side_references_[side]--;
    }
    notify(config_.notifier);
}

void channel::begin_operation() noexcept
{
    if (!valid_)
        return;
    lock_guard guard(*config_.synchronization);
    active_operations_++;
}

void channel::end_operation() noexcept
{
    if (!valid_)
        return;
    lock_guard guard(*config_.synchronization);
    active_operations_--;
}

bool channel::reserve_global(std::uint64_t bytes, std::uint64_t resources) noexcept
{
    return config_.owner_domain->reserve(bytes, resources);
}

void channel::release_global(std::uint64_t bytes, std::uint64_t resources) noexcept
{
    config_.owner_domain->release(bytes, resources);
}

na_status_t channel::enqueue(std::uint8_t sender, message &value, resource *resources, std::size_t resource_count,
                             std::uint64_t queue_message_limit) noexcept
{
    if (!valid_ || sender > 1 || !value.valid() || value.owner_ != this || value.resource_count() != 0 ||
        resource_count > value.resource_capacity() || (resource_count != 0 && resources == nullptr))
        return NA_STATUS_INVALID_ARGUMENT;
    if (value.byte_count() > max_bytes_ || resource_count > max_resources_)
        return NA_STATUS_INVALID_MESSAGE;
    auto &queue = queues_[1 - sender];
    {
        lock_guard guard(*config_.synchronization);
        const auto limit = queue_message_limit == 0 ? queue.capacity : queue_message_limit;
        if (side_references_[1 - sender] == 0)
            return NA_STATUS_PEER_CLOSED;
        if (queue.count >= limit || queue.count >= queue.capacity || value.byte_count() > max_bytes_ - queue.bytes ||
            resource_count > max_resources_ - queue.resources)
            return NA_STATUS_WOULD_BLOCK;
        if (!reserve_global(value.byte_count(), resource_count))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        for (std::size_t index = 0; index < resource_count; index++)
            value.resources_[index] = std::move(resources[index]);
        value.resource_count_ = resource_count;
        const auto tail = (queue.head + queue.count) % queue.capacity;
        queue.storage[tail] = &value;
        queue.count++;
        queue.bytes += value.byte_count();
        queue.resources += resource_count;
    }
    notify(config_.notifier);
    return NA_STATUS_OK;
}

na_status_t channel::claim_receive(std::uint8_t side, message *&value) noexcept
{
    value = nullptr;
    if (!valid_ || side > 1)
        return NA_STATUS_INVALID_ARGUMENT;
    lock_guard guard(*config_.synchronization);
    auto &queue = queues_[side];
    if (queue.claimed)
        return NA_STATUS_WOULD_BLOCK;
    if (queue.count == 0)
        return side_references_[1 - side] == 0 ? NA_STATUS_PEER_CLOSED : NA_STATUS_WOULD_BLOCK;
    queue.claimed = true;
    value = queue.storage[queue.head];
    active_claims_++;
    return NA_STATUS_OK;
}

bool channel::cancel_receive(std::uint8_t side, message &value) noexcept
{
    if (!valid_ || side > 1)
        return false;
    bool cancelled = false;
    {
        lock_guard guard(*config_.synchronization);
        auto &queue = queues_[side];
        cancelled = queue.claimed && queue.count != 0 && queue.storage[queue.head] == &value;
        if (cancelled)
        {
            queue.claimed = false;
            active_claims_--;
        }
    }
    if (cancelled)
        notify(config_.notifier);
    return cancelled;
}

bool channel::commit_receive(std::uint8_t side, message &value) noexcept
{
    if (!valid_ || side > 1)
        return false;
    bool committed = false;
    {
        lock_guard guard(*config_.synchronization);
        auto &queue = queues_[side];
        committed = queue.claimed && queue.count != 0 && queue.storage[queue.head] == &value;
        if (committed)
        {
            queue.storage[queue.head] = nullptr;
            queue.head = (queue.head + 1) % queue.capacity;
            queue.count--;
            queue.bytes -= value.byte_count();
            queue.resources -= value.resource_count();
            queue.claimed = false;
            active_claims_--;
            release_global(value.byte_count(), value.resource_count());
        }
    }
    if (committed)
        notify(config_.notifier);
    return committed;
}

bool channel::discard(std::uint8_t side, message *&value) noexcept
{
    value = nullptr;
    if (!valid_ || side > 1)
        return false;
    {
        lock_guard guard(*config_.synchronization);
        auto &queue = queues_[side];
        if (queue.count == 0 || queue.claimed)
            return false;
        value = queue.storage[queue.head];
        queue.storage[queue.head] = nullptr;
        queue.head = (queue.head + 1) % queue.capacity;
        queue.count--;
        queue.bytes -= value->byte_count();
        queue.resources -= value->resource_count();
        release_global(value->byte_count(), value->resource_count());
    }
    notify(config_.notifier);
    return value != nullptr;
}

void channel::visit_queued_messages(message_visitor visitor, void *context) const noexcept
{
    if (!valid_ || visitor == nullptr)
        return;
    lock_guard guard(*config_.synchronization);
    for (std::uint8_t side = 0; side < 2; side++)
    {
        const auto &queue = queues_[side];
        for (std::uint64_t index = 0; index < queue.count; index++)
        {
            auto *value = queue.storage[(queue.head + index) % queue.capacity];
            if (value != nullptr)
                visitor(context, *value);
        }
    }
}

namespace
{
/*
 * The ring control block and slot headers live in caller storage.  The
 * layout is self-describing through `magic`/`geometry` so a second address
 * space can attach the same bytes.  `produce` and `consume` sit on separate
 * cache lines because different threads write them on the steady-state path.
 */
constexpr std::uint64_t ring_magic = 0x4E414F5352494E47ULL; // "NAOSRING"

struct ring_slot_header
{
    std::atomic<std::uint64_t> sequence;
    std::uint64_t byte_count;
    std::uint64_t resource_count;
};

struct ring_control
{
    std::atomic<std::uint64_t> magic;
    std::atomic<std::uint64_t> closed;
    std::atomic<std::uint64_t> geometry;
    std::uint64_t padding0[5];
    std::atomic<std::uint64_t> produce;
    std::uint64_t padding1[7];
    std::atomic<std::uint64_t> consume;
    std::uint64_t padding2[7];
};

static_assert(sizeof(ring_control) == 192, "ring control block spans three cache lines");
static_assert(offsetof(ring_control, produce) == 64, "produce must not share a line with the header words");
static_assert(offsetof(ring_control, consume) == 128, "consume must not share a line with produce");

std::uint64_t round_up(std::uint64_t value, std::uint64_t alignment) noexcept
{
    return (value + alignment - 1) / alignment * alignment;
}

struct ring_layout
{
    std::uint64_t slots_offset;
    std::uint64_t payload_offset;
    std::uint64_t resource_offset;
    std::uint64_t stride;
    std::uint64_t total;
};

ring_layout ring_layout_for(std::uint64_t slot_bytes, std::uint32_t slot_capacity,
                            std::uint32_t resource_capacity) noexcept
{
    ring_layout layout{};
    layout.slots_offset = round_up(sizeof(ring_control), 8);
    layout.payload_offset = round_up(sizeof(ring_slot_header), 8);
    layout.resource_offset = round_up(layout.payload_offset + slot_bytes, 8);
    layout.stride = round_up(layout.resource_offset + sizeof(resource_record) * resource_capacity, 8);
    layout.total = layout.slots_offset + layout.stride * slot_capacity;
    return layout;
}

std::uint64_t ring_geometry(std::uint64_t slot_bytes, std::uint32_t slot_capacity,
                            std::uint32_t resource_capacity) noexcept
{
    return (slot_bytes & 0xFFFFFFFFULL) | (static_cast<std::uint64_t>(slot_capacity) << 32) |
           (static_cast<std::uint64_t>(resource_capacity) << 48);
}

bool ring_geometry_valid(std::uint64_t slot_bytes, std::uint32_t slot_capacity,
                         std::uint32_t resource_capacity) noexcept
{
    return slot_bytes != 0 && slot_bytes <= ring_max_slot_bytes && slot_capacity != 0 &&
           slot_capacity <= ring_max_slots && resource_capacity <= ring_max_resources_per_slot;
}
} // namespace

std::uint64_t ring::required_bytes(std::uint64_t slot_bytes, std::uint32_t slot_capacity,
                                   std::uint32_t resource_capacity) noexcept
{
    if (!ring_geometry_valid(slot_bytes, slot_capacity, resource_capacity))
        return 0;
    return ring_layout_for(slot_bytes, slot_capacity, resource_capacity).total;
}

na_status_t ring::format(void *storage, std::uint64_t storage_bytes, std::uint64_t slot_bytes,
                         std::uint32_t slot_capacity, std::uint32_t resource_capacity) noexcept
{
    if (storage == nullptr || !ring_geometry_valid(slot_bytes, slot_capacity, resource_capacity))
        return NA_STATUS_INVALID_ARGUMENT;
    if (reinterpret_cast<std::uintptr_t>(storage) % alignof(ring_control) != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    const auto layout = ring_layout_for(slot_bytes, slot_capacity, resource_capacity);
    if (storage_bytes < layout.total)
        return NA_STATUS_BUFFER_TOO_SMALL;

    auto *control = reinterpret_cast<ring_control *>(storage);
    const auto geometry = ring_geometry(slot_bytes, slot_capacity, resource_capacity);
    if (control->magic.load(std::memory_order_acquire) == ring_magic)
        return control->geometry.load(std::memory_order_acquire) == geometry ? NA_STATUS_OK
                                                                            : NA_STATUS_INVALID_ARGUMENT;

    ::new (storage) ring_control();
    control->magic.store(0, std::memory_order_relaxed);
    control->closed.store(0, std::memory_order_relaxed);
    control->geometry.store(geometry, std::memory_order_relaxed);
    control->produce.store(0, std::memory_order_relaxed);
    control->consume.store(0, std::memory_order_relaxed);
    auto *slots = static_cast<std::uint8_t *>(storage) + layout.slots_offset;
    for (std::uint64_t index = 0; index < slot_capacity; index++)
    {
        auto *slot = slots + index * layout.stride;
        ::new (static_cast<void *>(slot)) ring_slot_header();
        auto *header = reinterpret_cast<ring_slot_header *>(slot);
        header->sequence.store(index, std::memory_order_relaxed);
        header->byte_count = 0;
        header->resource_count = 0;
        auto *records = reinterpret_cast<resource_record *>(slot + layout.resource_offset);
        for (std::uint32_t resource = 0; resource < resource_capacity; resource++)
            records[resource].clear();
    }
    // Publish the formatted control block last so a side that observes the
    // magic with acquire also observes every initialized slot.
    control->magic.store(ring_magic, std::memory_order_release);
    return NA_STATUS_OK;
}

ring::ring(const ring_config &config) noexcept
    : control_allocator_(*config.control_memory)
    , lock_(*config.synchronization)
    , notifier_(config.notifier)
    , storage_(static_cast<std::uint8_t *>(config.storage))
    , control_(config.storage)
    , slot_bytes_(config.slot_bytes)
    , slot_capacity_(config.slot_capacity)
    , resource_capacity_(config.resource_capacity)
    , valid_(true)
{
    const auto layout = ring_layout_for(config.slot_bytes, config.slot_capacity, config.resource_capacity);
    slots_offset_ = layout.slots_offset;
    payload_offset_ = layout.payload_offset;
    resource_offset_ = layout.resource_offset;
    slot_stride_ = layout.stride;
}

ring *ring::create(const ring_config &config) noexcept
{
    if (!valid_allocator(config.control_memory) || !valid_lock(config.synchronization) || config.storage == nullptr ||
        !ring_geometry_valid(config.slot_bytes, config.slot_capacity, config.resource_capacity))
        return nullptr;
    const auto layout = ring_layout_for(config.slot_bytes, config.slot_capacity, config.resource_capacity);
    if (config.storage_bytes < layout.total ||
        reinterpret_cast<std::uintptr_t>(config.storage) % alignof(ring_control) != 0)
        return nullptr;
    auto *control = reinterpret_cast<ring_control *>(config.storage);
    if (control->magic.load(std::memory_order_acquire) != ring_magic ||
        control->geometry.load(std::memory_order_acquire) !=
            ring_geometry(config.slot_bytes, config.slot_capacity, config.resource_capacity))
        return nullptr;
    void *storage = config.control_memory->allocate(sizeof(ring), alignof(ring));
    if (storage == nullptr)
        return nullptr;
    return ::new (storage) ring(config);
}

void ring::destroy() noexcept
{
    allocator &memory = control_allocator_;
    {
        lock_guard guard(lock_);
        valid_ = false;
    }
    // The storage is caller-owned, but resources still resident in it belong
    // to the transport.  Release them so a quiesced teardown cannot leak an
    // adapter's handles.  Only a ring whose sides have stopped may be
    // destroyed.
    for (std::uint64_t index = 0; index < slot_capacity_; index++)
        release_slot_resources(index);
    this->~ring();
    memory.deallocate(this, sizeof(ring), alignof(ring));
}

std::uint8_t *ring::slot_base(std::uint64_t index) const noexcept
{
    return storage_ + slots_offset_ + index * slot_stride_;
}

void ring::release_slot_resources(std::uint64_t index) noexcept
{
    auto *records = reinterpret_cast<resource_record *>(slot_base(index) + resource_offset_);
    for (std::uint32_t resource = 0; resource < resource_capacity_; resource++)
    {
        if (records[resource].valid())
        {
            if (records[resource].release != nullptr)
                records[resource].release(records[resource].context, records[resource].value);
            records[resource].clear();
        }
    }
}

std::uint64_t ring::queued() const noexcept
{
    if (!valid_)
        return 0;
    const auto *control = static_cast<const ring_control *>(control_);
    const std::uint64_t produced = control->produce.load(std::memory_order_acquire);
    const std::uint64_t consumed = control->consume.load(std::memory_order_acquire);
    return produced > consumed ? produced - consumed : 0;
}

std::uint64_t ring::signals(std::uint8_t side) const noexcept
{
    if (!valid_ || side > 1)
        return 0;
    const auto *control = static_cast<const ring_control *>(control_);
    const std::uint64_t closed = control->closed.load(std::memory_order_acquire);
    std::uint64_t result = 0;
    if (side == 0)
    {
        if ((closed & 0x2ULL) != 0)
            return NA_SIGNAL_PEER_CLOSED;
        if (queued() < slot_capacity_)
            result |= NA_SIGNAL_WRITABLE;
    }
    else
    {
        if (queued() != 0)
            result |= NA_SIGNAL_READABLE;
        if ((closed & 0x1ULL) != 0)
            result |= NA_SIGNAL_PEER_CLOSED;
    }
    return result;
}

na_status_t ring::send(const std::uint8_t *bytes, std::uint64_t byte_count) noexcept
{
    return send(bytes, byte_count, nullptr, 0);
}

na_status_t ring::send(const std::uint8_t *bytes, std::uint64_t byte_count, resource_record *resources,
                       std::size_t resource_count) noexcept
{
    if (!valid_ || (byte_count != 0 && bytes == nullptr) || (resource_count != 0 && resources == nullptr))
        return NA_STATUS_INVALID_ARGUMENT;
    if (byte_count > slot_bytes_ || resource_count > resource_capacity_)
        return NA_STATUS_INVALID_MESSAGE;
    for (std::size_t resource = 0; resource < resource_count; resource++)
    {
        if (!resources[resource].valid() || resources[resource].release == nullptr)
            return NA_STATUS_INVALID_ARGUMENT;
    }
    auto *control = static_cast<ring_control *>(control_);
    if ((control->closed.load(std::memory_order_acquire) & 0x2ULL) != 0)
        return NA_STATUS_PEER_CLOSED;
    const std::uint64_t position = control->produce.load(std::memory_order_relaxed);
    const std::uint64_t consumed = control->consume.load(std::memory_order_acquire);
    if (position - consumed >= slot_capacity_)
        return NA_STATUS_WOULD_BLOCK;
    const std::uint64_t index = position % slot_capacity_;
    auto *header = reinterpret_cast<ring_slot_header *>(slot_base(index));
    // Reclaim edge: acquire pairs with the consumer's release store of
    // `position` when it stopped pinning this slot.  Any other value means
    // the slot is still pinned or the release is not visible yet.
    if (header->sequence.load(std::memory_order_acquire) != position)
        return NA_STATUS_WOULD_BLOCK;
    header->byte_count = byte_count;
    header->resource_count = resource_count;
    if (byte_count != 0)
        std::memcpy(slot_base(index) + payload_offset_, bytes, byte_count);
    auto *records = reinterpret_cast<resource_record *>(slot_base(index) + resource_offset_);
    for (std::size_t resource = 0; resource < resource_count; resource++)
    {
        records[resource] = resources[resource];
        resources[resource].clear();
    }
    // Publish edge: every store above precedes this release, and the
    // consumer's acquire load of the same word observes all of it.
    header->sequence.store(position + 1, std::memory_order_release);
    control->produce.store(position + 1, std::memory_order_relaxed);
    notify(notifier_);
    return NA_STATUS_OK;
}

na_status_t ring::claim(std::uint8_t *destination, std::uint64_t byte_capacity, std::uint64_t &byte_count,
                        std::uint64_t &resource_count) noexcept
{
    byte_count = 0;
    resource_count = 0;
    if (!valid_ || (byte_capacity != 0 && destination == nullptr))
        return NA_STATUS_INVALID_ARGUMENT;
    if (claimed_)
        return NA_STATUS_WOULD_BLOCK;
    auto *control = static_cast<ring_control *>(control_);
    const std::uint64_t position = control->consume.load(std::memory_order_relaxed);
    const std::uint64_t index = position % slot_capacity_;
    auto *header = reinterpret_cast<ring_slot_header *>(slot_base(index));
    // Publish edge: acquire pairs with the producer's release store.
    if (header->sequence.load(std::memory_order_acquire) != position + 1)
    {
        if ((control->closed.load(std::memory_order_acquire) & 0x1ULL) == 0)
            return NA_STATUS_WOULD_BLOCK;
        // The close release-store happens-before this acquire, so a publish
        // that raced with the close is now visible; only report the close
        // when the re-check still finds nothing, otherwise the entry would
        // be lost.
        if (header->sequence.load(std::memory_order_acquire) != position + 1)
            return NA_STATUS_PEER_CLOSED;
    }
    const std::uint64_t length = header->byte_count;
    const std::uint64_t records = header->resource_count;
    if (length > byte_capacity)
    {
        byte_count = length;
        resource_count = records;
        return NA_STATUS_BUFFER_TOO_SMALL;
    }
    // Copy the payload out before returning: the consumer validates its own
    // copy, and the slot stays pinned until commit/cancel.
    if (length != 0)
        std::memcpy(destination, slot_base(index) + payload_offset_, length);
    claimed_ = true;
    claimed_position_ = position;
    claimed_resources_ = records;
    byte_count = length;
    resource_count = records;
    return NA_STATUS_OK;
}

na_status_t ring::take_resource(std::uint64_t index, resource_record &result) noexcept
{
    if (!valid_)
        return NA_STATUS_INVALID_ARGUMENT;
    if (!claimed_)
        return NA_STATUS_INVALID_HANDLE;
    if (index >= claimed_resources_)
        return NA_STATUS_INVALID_ARGUMENT;
    auto *records =
        reinterpret_cast<resource_record *>(slot_base(claimed_position_ % slot_capacity_) + resource_offset_);
    if (!records[index].valid())
        return NA_STATUS_INVALID_HANDLE;
    result = records[index];
    records[index].clear();
    return NA_STATUS_OK;
}

na_status_t ring::commit() noexcept
{
    if (!valid_ || !claimed_)
        return NA_STATUS_INVALID_HANDLE;
    auto *control = static_cast<ring_control *>(control_);
    const std::uint64_t index = claimed_position_ % slot_capacity_;
    // Resources the consumer did not take must not leak.
    release_slot_resources(index);
    auto *header = reinterpret_cast<ring_slot_header *>(slot_base(index));
    // Reclaim edge: the slot becomes recyclable only after the payload was
    // copied out and every resident resource was taken or released.
    header->sequence.store(claimed_position_ + slot_capacity_, std::memory_order_release);
    control->consume.store(claimed_position_ + 1, std::memory_order_relaxed);
    claimed_ = false;
    claimed_resources_ = 0;
    notify(notifier_);
    return NA_STATUS_OK;
}

na_status_t ring::cancel() noexcept
{
    // A cancelled claim discards the pinned entry instead of rewinding the
    // queue: the payload was already copied out, and the slot must become
    // recyclable or the producer would stall forever.
    return commit();
}

void ring::close(std::uint8_t side) noexcept
{
    if (!valid_ || side > 1)
        return;
    auto *control = static_cast<ring_control *>(control_);
    control->closed.fetch_or(0x1ULL << side, std::memory_order_acq_rel);
    notify(notifier_);
}

} // namespace naos::ipc_core
