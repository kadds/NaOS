/*
 * C ABI fragment for the optional SPSC ring.  It only adapts platform
 * callbacks and copies C records into/out of the core's record type; the
 * queue/claim/commit state machine itself lives in ring::* and is never
 * duplicated here.
 */

namespace
{

struct ring_storage
{
    allocator_adapter control_memory;
    lock_adapter synchronization;
    notifier_adapter notifier;
    clock_adapter clock;
    handle_table_adapter handles;
    core_ring *ring;

    explicit ring_storage(const naos_ipc_ring_config_t &config)
        : control_memory(*config.control_memory)
        , synchronization(*config.synchronization)
        , notifier(config.notifier)
        , clock(config.clock)
        , handles(config.handles)
        , ring(nullptr)
    {
        // Scalar stores mirror the channel adapter: the kernel entry path is
        // not assumed to provide the hosted ABI's 16-byte stack alignment,
        // and aggregate initialization of the adapters can lower to movaps.
        naos::ipc_core::ring_config native_config;
        native_config.control_memory = &control_memory;
        native_config.synchronization = &synchronization;
        native_config.notifier = &notifier;
        native_config.timer = &clock;
        native_config.handles = &handles;
        native_config.storage = config.storage;
        native_config.storage_bytes = config.storage_bytes;
        native_config.slot_bytes = config.slot_bytes;
        native_config.slot_capacity = config.slot_capacity;
        native_config.resource_capacity = config.resource_capacity;
        ring = core_ring::create(native_config);
    }
};

bool valid_ring_config(const naos_ipc_ring_config_t *config) noexcept
{
    return config != nullptr && valid_allocator_api(config->control_memory) &&
           valid_lock_api(config->synchronization) && config->storage != nullptr && config->storage_bytes != 0 &&
           config->slot_bytes != 0 && config->slot_capacity != 0;
}

} // namespace

struct naos_ipc_ring : ring_storage
{
    explicit naos_ipc_ring(const naos_ipc_ring_config_t &config)
        : ring_storage(config)
    {
    }
};

extern "C" {

uint64_t naos_ipc_ring_required_bytes(uint64_t slot_bytes, uint32_t slot_capacity, uint32_t resource_capacity)
{
    return core_ring::required_bytes(slot_bytes, slot_capacity, resource_capacity);
}

naos_ipc_status_t naos_ipc_ring_init(void *storage, uint64_t storage_bytes, uint64_t slot_bytes,
                                     uint32_t slot_capacity, uint32_t resource_capacity)
{
    return static_cast<naos_ipc_status_t>(
        core_ring::format(storage, storage_bytes, slot_bytes, slot_capacity, resource_capacity));
}

naos_ipc_ring_t *naos_ipc_ring_create(const naos_ipc_ring_config_t *config)
{
    if (!valid_ring_config(config))
        return nullptr;
    void *storage = config->control_memory->allocate(config->control_memory->context, sizeof(naos_ipc_ring_t),
                                                     alignof(naos_ipc_ring_t));
    if (storage == nullptr)
        return nullptr;
    auto *result = ::new (storage) naos_ipc_ring_t(*config);
    if (result->ring == nullptr)
    {
        result->~naos_ipc_ring_t();
        config->control_memory->deallocate(config->control_memory->context, storage, sizeof(naos_ipc_ring_t),
                                           alignof(naos_ipc_ring_t));
        return nullptr;
    }
    return result;
}

void naos_ipc_ring_destroy(naos_ipc_ring_t *ring)
{
    if (ring == nullptr || ring->ring == nullptr)
        return;
    auto &memory = ring->control_memory;
    ring->ring->destroy();
    ring->ring = nullptr;
    ring->~naos_ipc_ring_t();
    memory.deallocate(ring, sizeof(naos_ipc_ring_t), alignof(naos_ipc_ring_t));
}

int naos_ipc_ring_valid(const naos_ipc_ring_t *ring)
{
    return ring != nullptr && ring->ring != nullptr && ring->ring->valid();
}

uint64_t naos_ipc_ring_slot_capacity(const naos_ipc_ring_t *ring)
{
    return naos_ipc_ring_valid(ring) ? ring->ring->slot_capacity() : 0;
}

uint64_t naos_ipc_ring_slot_bytes(const naos_ipc_ring_t *ring)
{
    return naos_ipc_ring_valid(ring) ? ring->ring->slot_bytes() : 0;
}

uint64_t naos_ipc_ring_resource_capacity(const naos_ipc_ring_t *ring)
{
    return naos_ipc_ring_valid(ring) ? ring->ring->resource_capacity() : 0;
}

uint64_t naos_ipc_ring_queued(const naos_ipc_ring_t *ring)
{
    return naos_ipc_ring_valid(ring) ? ring->ring->queued() : 0;
}

int naos_ipc_ring_claim_pending(const naos_ipc_ring_t *ring)
{
    return naos_ipc_ring_valid(ring) && ring->ring->claim_pending();
}

naos_ipc_signal_t naos_ipc_ring_signals(const naos_ipc_ring_t *ring, uint8_t side)
{
    return naos_ipc_ring_valid(ring) ? ring->ring->signals(side) : 0;
}

naos_ipc_status_t naos_ipc_ring_send(naos_ipc_ring_t *ring, const uint8_t *bytes, uint64_t byte_count,
                                     naos_ipc_resource_t *resources, size_t resource_count)
{
    if (!naos_ipc_ring_valid(ring) || (byte_count != 0 && bytes == nullptr) ||
        (resource_count != 0 && resources == nullptr))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    if (resource_count > NAOS_IPC_RING_MAX_RESOURCES_PER_SLOT)
        return NAOS_IPC_STATUS_INVALID_MESSAGE;
    core_resource_record records[NAOS_IPC_RING_MAX_RESOURCES_PER_SLOT];
    for (size_t index = 0; index < resource_count; index++)
    {
        records[index].context = resources[index].context;
        records[index].value = resources[index].value;
        records[index].release = resources[index].release;
    }
    const auto status = ring->ring->send(bytes, byte_count, records, resource_count);
    if (status == NA_STATUS_OK)
    {
        for (size_t index = 0; index < resource_count; index++)
            clear_resource(&resources[index]);
    }
    return static_cast<naos_ipc_status_t>(status);
}

naos_ipc_status_t naos_ipc_ring_claim(naos_ipc_ring_t *ring, uint8_t *destination, uint64_t byte_capacity,
                                      uint64_t *byte_count, uint64_t *resource_count)
{
    if (!naos_ipc_ring_valid(ring) || byte_count == nullptr || resource_count == nullptr)
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    return static_cast<naos_ipc_status_t>(
        ring->ring->claim(destination, byte_capacity, *byte_count, *resource_count));
}

naos_ipc_status_t naos_ipc_ring_take_resource(naos_ipc_ring_t *ring, uint64_t index, naos_ipc_resource_t *resource)
{
    if (!naos_ipc_ring_valid(ring) || resource == nullptr || naos_ipc_resource_valid(resource))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    core_resource_record record;
    const auto status = ring->ring->take_resource(index, record);
    if (status != NA_STATUS_OK)
        return static_cast<naos_ipc_status_t>(status);
    resource->context = record.context;
    resource->value = record.value;
    resource->release = record.release;
    record.clear();
    return NAOS_IPC_STATUS_OK;
}

naos_ipc_status_t naos_ipc_ring_commit(naos_ipc_ring_t *ring)
{
    if (!naos_ipc_ring_valid(ring))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    return static_cast<naos_ipc_status_t>(ring->ring->commit());
}

naos_ipc_status_t naos_ipc_ring_cancel(naos_ipc_ring_t *ring)
{
    if (!naos_ipc_ring_valid(ring))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    return static_cast<naos_ipc_status_t>(ring->ring->cancel());
}

void naos_ipc_ring_close(naos_ipc_ring_t *ring, uint8_t side)
{
    if (naos_ipc_ring_valid(ring))
        ring->ring->close(side);
}

} // extern "C"
