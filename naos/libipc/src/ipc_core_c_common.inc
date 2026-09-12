#include <naos/ipc_core.h>

#include <new>
#include <utility>

#include <naos/ipc_core.hpp>

namespace
{
using core_allocator = naos::ipc_core::allocator;
using core_channel = naos::ipc_core::channel;
using core_domain = naos::ipc_core::domain;
using core_invocation = naos::ipc_core::invocation;
using core_message = naos::ipc_core::message;
using core_resource = naos::ipc_core::resource;
using core_resource_record = naos::ipc_core::resource_record;
using core_ring = naos::ipc_core::ring;

/* The C header and the C++ core must agree on the transport limits. */
static_assert(NAOS_IPC_RING_MAX_SLOTS == naos::ipc_core::ring_max_slots, "slot limit drift");
static_assert(NAOS_IPC_RING_MAX_SLOT_BYTES == naos::ipc_core::ring_max_slot_bytes, "slot byte limit drift");
static_assert(NAOS_IPC_RING_MAX_RESOURCES_PER_SLOT == naos::ipc_core::ring_max_resources_per_slot,
              "resource limit drift");
/* The C ABI resource struct and the ring's storage record are interchangeable
 * by construction, so send/take cross the boundary without copying layout. */
static_assert(sizeof(naos_ipc_resource_t) == sizeof(core_resource_record), "resource record layout drift");
static_assert(offsetof(naos_ipc_resource_t, context) == offsetof(core_resource_record, context),
              "resource context offset drift");
static_assert(offsetof(naos_ipc_resource_t, value) == offsetof(core_resource_record, value),
              "resource value offset drift");
static_assert(offsetof(naos_ipc_resource_t, release) == offsetof(core_resource_record, release),
              "resource release offset drift");

class allocator_adapter final : public core_allocator
{
  public:
    explicit allocator_adapter(const naos_ipc_allocator_t &api)
        : api_(api)
    {
    }

    void *allocate(std::size_t size, std::size_t alignment) noexcept override
    {
        return api_.allocate == nullptr ? nullptr : api_.allocate(api_.context, size, alignment);
    }

    void deallocate(void *pointer, std::size_t size, std::size_t alignment) noexcept override
    {
        if (api_.deallocate != nullptr)
            api_.deallocate(api_.context, pointer, size, alignment);
    }

  private:
    naos_ipc_allocator_t api_;
};

class lock_adapter final : public naos::ipc_core::lock
{
  public:
    explicit lock_adapter(const naos_ipc_lock_t &api)
        : api_(api)
    {
    }

    void acquire() noexcept override
    {
        if (api_.acquire != nullptr)
            api_.acquire(api_.context);
    }

    void release() noexcept override
    {
        if (api_.release != nullptr)
            api_.release(api_.context);
    }

  private:
    naos_ipc_lock_t api_;
};

class notifier_adapter final : public naos::ipc_core::wait_notifier
{
  public:
    explicit notifier_adapter(const naos_ipc_wait_notifier_t *api)
        : api_(api == nullptr ? naos_ipc_wait_notifier_t{} : *api)
    {
    }

    void notify() noexcept override
    {
        if (api_.notify != nullptr)
            api_.notify(api_.context);
    }

  private:
    naos_ipc_wait_notifier_t api_;
};

class clock_adapter final : public naos::ipc_core::clock
{
  public:
    explicit clock_adapter(const naos_ipc_clock_t *api)
        : api_(api == nullptr ? naos_ipc_clock_t{} : *api)
    {
    }

    std::uint64_t now() noexcept override { return api_.now == nullptr ? 0 : api_.now(api_.context); }

  private:
    naos_ipc_clock_t api_;
};

class handle_table_adapter final : public naos::ipc_core::handle_table
{
  public:
    explicit handle_table_adapter(const naos_ipc_handle_table_t *api)
        : api_(api == nullptr ? naos_ipc_handle_table_t{} : *api)
    {
    }

    na_status_t validate_waitable(std::uint64_t handle) noexcept override
    {
        return static_cast<na_status_t>(api_.validate_waitable == nullptr
                                            ? NAOS_IPC_STATUS_INVALID_HANDLE
                                            : api_.validate_waitable(api_.context, handle));
    }

    na_status_t begin_transfer(const std::uint64_t *handles, std::size_t count, void *transaction) noexcept override
    {
        if (api_.begin_transfer == nullptr)
            return NA_STATUS_NOT_SUPPORTED;
        void *candidate = transaction;
        return static_cast<na_status_t>(api_.begin_transfer(api_.context, handles, count, &candidate));
    }

    na_status_t restore_transfer(void *transaction) noexcept override
    {
        return static_cast<na_status_t>(api_.restore_transfer == nullptr
                                            ? NAOS_IPC_STATUS_NOT_SUPPORTED
                                            : api_.restore_transfer(api_.context, transaction));
    }

    void commit_transfer(void *transaction) noexcept override
    {
        if (api_.commit_transfer != nullptr)
            api_.commit_transfer(api_.context, transaction);
    }

  private:
    naos_ipc_handle_table_t api_;
};

bool valid_allocator_api(const naos_ipc_allocator_t *api) noexcept
{
    return api != nullptr && api->allocate != nullptr && api->deallocate != nullptr;
}

bool valid_lock_api(const naos_ipc_lock_t *api) noexcept
{
    return api != nullptr && api->acquire != nullptr && api->release != nullptr;
}

void clear_resource(naos_ipc_resource_t *resource) noexcept
{
    if (resource == nullptr)
        return;
    resource->context = nullptr;
    resource->value = nullptr;
    resource->release = nullptr;
}

void copy_resource(naos_ipc_resource_t *destination, const core_resource &source) noexcept
{
    destination->context = source.context();
    destination->value = source.value();
    destination->release = source.release_function();
}

struct channel_storage;

struct message_storage
{
    channel_storage *owner;
    core_message *message;
    void *user_context;
};

struct domain_storage
{
    allocator_adapter memory;
    lock_adapter synchronization;
    core_domain *domain;

    explicit domain_storage(const naos_ipc_domain_config_t &config)
        : memory(*config.memory)
        , synchronization(*config.synchronization)
        , domain(nullptr)
    {
        alignas(16) naos::ipc_core::domain_config native_config{
            &memory, &synchronization, config.max_messages, config.max_bytes, config.max_resources,
        };
        domain = core_domain::create(native_config);
    }
};

} // namespace

/* The opaque C structs contain C++ adapters only in this implementation. */
struct naos_ipc_domain : domain_storage
{
    explicit naos_ipc_domain(const naos_ipc_domain_config_t &config)
        : domain_storage(config)
    {
    }
};

namespace
{

struct channel_storage
{
    allocator_adapter control_memory;
    allocator_adapter payload_memory;
    lock_adapter synchronization;
    notifier_adapter notifier;
    clock_adapter clock;
    handle_table_adapter handles;
    core_channel *channel;

    channel_storage(const naos_ipc_channel_config_t &config, core_domain *owner_domain)
        : control_memory(*config.control_memory)
        , payload_memory(*config.payload_memory)
        , synchronization(*config.synchronization)
        , notifier(config.notifier)
        , clock(config.clock)
        , handles(config.handles)
        , channel(nullptr)
    {
        alignas(16) naos::ipc_core::channel_config native_config{
            owner_domain, &control_memory, &payload_memory,     &synchronization, &notifier,
            &clock,       &handles,        config.max_messages, config.max_bytes, config.max_resources,
        };
        channel = core_channel::create(native_config);
    }
};

} // namespace

struct naos_ipc_channel : channel_storage
{
    explicit naos_ipc_channel(const naos_ipc_channel_config_t &config)
        : channel_storage(config, config.owner_domain == nullptr ? nullptr : config.owner_domain->domain)
    {
    }
};

struct naos_ipc_message : message_storage
{
    explicit naos_ipc_message(channel_storage *owner)
        : message_storage{owner, nullptr, nullptr}
    {
    }
};

struct invocation_storage;

namespace
{
bool remove_queued_invocation(void *context) noexcept;
void wake_invocation_execution(void *context) noexcept;
} // namespace

struct invocation_storage
{
    allocator_adapter control_memory;
    allocator_adapter payload_memory;
    lock_adapter synchronization;
    notifier_adapter notifier;
    clock_adapter clock;
    naos_ipc_invocation_callbacks_t callbacks;
    core_invocation *invocation;

    invocation_storage(const naos_ipc_invocation_config_t &config, core_domain *owner_domain)
        : control_memory(*config.control_memory)
        , payload_memory(*config.payload_memory)
        , synchronization(*config.synchronization)
        , notifier(config.notifier)
        , clock(config.clock)
        , callbacks(config.callbacks == nullptr ? naos_ipc_invocation_callbacks_t{} : *config.callbacks)
        , invocation(nullptr)
    {
        // Do not aggregate-initialize this local.  The kernel syscall entry
        // path is not guaranteed to enter C++ with the hosted ABI's 16-byte
        // stack alignment, and GCC lowers zero-initialization of the nested
        // callback aggregate to `movaps`.  Scalar stores keep this C ABI
        // adapter freestanding-safe; the core validates every field it uses.
        naos::ipc_core::invocation_config native_config;
        native_config.owner_domain = owner_domain;
        native_config.control_memory = &control_memory;
        native_config.payload_memory = &payload_memory;
        native_config.synchronization = &synchronization;
        native_config.notifier = &notifier;
        native_config.timer = &clock;
        native_config.callbacks.context = this;
        native_config.callbacks.remove_queued = remove_queued_invocation;
        native_config.callbacks.wake_execution = wake_invocation_execution;
        native_config.method_id = config.method_id;
        native_config.operation_deadline = config.operation_deadline;
        native_config.max_response_bytes = config.max_response_bytes;
        native_config.max_response_resources = config.max_response_resources;
        invocation = core_invocation::create(native_config);
    }
};

struct naos_ipc_invocation : invocation_storage
{
    explicit naos_ipc_invocation(const naos_ipc_invocation_config_t &config)
        : invocation_storage(config, config.owner_domain == nullptr ? nullptr : config.owner_domain->domain)
    {
    }
};

namespace
{
bool valid_domain_config(const naos_ipc_domain_config_t *config) noexcept
{
    return config != nullptr && valid_allocator_api(config->memory) && valid_lock_api(config->synchronization) &&
           config->max_messages != 0 && config->max_bytes != 0 && config->max_resources != 0;
}

bool valid_channel_config(const naos_ipc_channel_config_t *config) noexcept
{
    return config != nullptr && config->owner_domain != nullptr && config->owner_domain->domain != nullptr &&
           config->owner_domain->domain->valid() && valid_allocator_api(config->control_memory) &&
           valid_allocator_api(config->payload_memory) && valid_lock_api(config->synchronization) &&
           config->max_messages != 0 && config->max_bytes != 0 && config->max_resources != 0;
}

bool valid_invocation_config(const naos_ipc_invocation_config_t *config) noexcept
{
    return config != nullptr && config->owner_domain != nullptr && config->owner_domain->domain != nullptr &&
           config->owner_domain->domain->valid() && valid_allocator_api(config->control_memory) &&
           valid_allocator_api(config->payload_memory) && valid_lock_api(config->synchronization) &&
           config->max_response_bytes <= NA_CHANNEL_MAX_MESSAGE_BYTES &&
           config->max_response_resources <= NA_CHANNEL_MAX_RESOURCES;
}

bool c_resources_to_core(allocator_adapter &memory, naos_ipc_resource_t *resources, std::size_t count,
                         core_resource *&converted, bool allow_empty) noexcept
{
    converted = nullptr;
    if (count == 0)
        return true;
    if (resources == nullptr)
        return false;
    for (std::size_t index = 0; index < count; index++)
    {
        const bool empty = resources[index].context == nullptr && resources[index].value == nullptr &&
                           resources[index].release == nullptr;
        if (empty && allow_empty)
            continue;
        if (!naos_ipc_resource_valid(&resources[index]) || resources[index].release == nullptr)
            return false;
    }
    converted = static_cast<core_resource *>(memory.allocate(sizeof(core_resource) * count, alignof(core_resource)));
    if (converted == nullptr)
        return false;
    for (std::size_t index = 0; index < count; index++)
    {
        const bool empty = resources[index].context == nullptr && resources[index].value == nullptr &&
                           resources[index].release == nullptr;
        if (empty)
            ::new (converted + index) core_resource();
        else
        {
            ::new (converted + index)
                core_resource(resources[index].context, resources[index].value, resources[index].release);
            clear_resource(&resources[index]);
        }
    }
    return true;
}

void restore_core_resources(allocator_adapter &memory, core_resource *converted, naos_ipc_resource_t *resources,
                            std::size_t count) noexcept
{
    if (converted == nullptr)
        return;
    for (std::size_t index = 0; index < count; index++)
    {
        if (converted[index].valid())
        {
            copy_resource(&resources[index], converted[index]);
            converted[index].take_value();
        }
        converted[index].~resource();
    }
    memory.deallocate(converted, sizeof(core_resource) * count, alignof(core_resource));
}

bool remove_queued_invocation(void *context) noexcept
{
    auto *storage = static_cast<invocation_storage *>(context);
    return storage->callbacks.remove_queued != nullptr &&
           storage->callbacks.remove_queued(storage->callbacks.context) != 0;
}

void wake_invocation_execution(void *context) noexcept
{
    auto *storage = static_cast<invocation_storage *>(context);
    if (storage->callbacks.wake_execution != nullptr)
        storage->callbacks.wake_execution(storage->callbacks.context);
}

void destroy_message_without_wrapper(core_message *message) noexcept
{
    if (message != nullptr)
        message->destroy();
}

struct queued_message_visitor_context
{
    naos_ipc_message_visitor_fn visitor;
    void *context;
};

void visit_queued_message(void *raw_context, core_message &message) noexcept
{
    auto *context = static_cast<queued_message_visitor_context *>(raw_context);
    auto *wrapper = static_cast<naos_ipc_message_t *>(message.user_context());
    if (wrapper != nullptr && context->visitor != nullptr)
        context->visitor(context->context, wrapper);
}
} // namespace

