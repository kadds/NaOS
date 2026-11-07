#include "kernel/pci/dma_domain.hpp"

#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"

KLOG_MODULE(kernel);

namespace pci
{
namespace
{
constexpr u32 quarantined_backing_capacity = 512;
std::atomic_flag quarantine_lock = ATOMIC_FLAG_INIT;
using backing_handle = handle_t<naos::data_plane::memory_object>;
alignas(
    backing_handle) unsigned char quarantined_backing_storage[sizeof(backing_handle) * quarantined_backing_capacity]{};
bool quarantined_backing_used[quarantined_backing_capacity]{};

void quarantine_backing(handle_t<naos::data_plane::memory_object> backing)
{
    if (!backing)
        return;
    while (quarantine_lock.test_and_set(std::memory_order_acquire))
        cpu_pause();

    for (u32 index = 0; index < quarantined_backing_capacity; ++index)
    {
        if (!quarantined_backing_used[index])
        {
            auto *slot =
                new (quarantined_backing_storage + sizeof(backing_handle) * index) backing_handle(std::move(backing));
            (void)slot;
            quarantined_backing_used[index] = true;
            quarantine_lock.clear(std::memory_order_release);
            return;
        }
    }

    // Retain the reference count even when the bounded quarantine is full.
    // This intentionally leaks the object rather than freeing a frame that
    // hardware may still be able to address.
    backing.clear_control();
    quarantine_lock.clear(std::memory_order_release);
}

na_status_t map_error_status(dma_mapping_error error)
{
    switch (error)
    {
        case dma_mapping_error::none:
            return NA_STATUS_OK;
        case dma_mapping_error::invalid_address:
        case dma_mapping_error::already_present:
        case dma_mapping_error::invalid_slot:
        case dma_mapping_error::invalid_transition:
        case dma_mapping_error::not_found:
            return NA_STATUS_INVALID_ARGUMENT;
        case dma_mapping_error::address_width_exceeded:
            return NA_STATUS_NOT_SUPPORTED;
        case dma_mapping_error::capacity_exhausted:
            return NA_STATUS_RESOURCE_EXHAUSTED;
        case dma_mapping_error::invalid_resource:
            return NA_STATUS_INVALID_HANDLE;
        case dma_mapping_error::bus_master_enabled:
            return NA_STATUS_ACCESS_DENIED;
        case dma_mapping_error::state_uncertain:
            return NA_STATUS_IO_ERROR;
    }
    return NA_STATUS_INVALID_ARGUMENT;
}
} // namespace

dma_domain::dma_domain(process_id owner, function_identity identity, backend_ops operations, void *context,
                       void *backend_state, configuration config, physical_resource::root *resource_root)
    : kobject(type_of())
    , owner_(owner)
    , identity_(identity)
    , operations_(operations)
    , context_(context)
    , backend_state_(backend_state)
    , configuration_(config)
    , resource_root_(resource_root)
    , state_uncertain_(config.state_uncertain)
{
}

dma_domain::~dma_domain() { teardown_noexcept(); }

dma_operation_result dma_domain::map_page(process_id caller, handle_t<naos::data_plane::memory_object> backing,
                                          u64 iova, u64 object_offset, naos::data_plane::dma_direction direction)
{
    if (caller != owner_)
        return NA_STATUS_ACCESS_DENIED;
    operation_guard operation(operation_in_progress_);
    if (!operation)
        return NA_STATUS_WOULD_BLOCK;
    if (closed_)
        return NA_STATUS_OBJECT_REVOKED;
    if (state_uncertain_.load(std::memory_order_acquire))
        return NA_STATUS_IO_ERROR;
    if (!configuration_.domain_ready || (!configuration_.iommu_attached && !configuration_.trusted_driver_override))
        return NA_STATUS_ACCESS_DENIED;
    if (bus_master_enabled_)
        return NA_STATUS_ACCESS_DENIED;
    if (!backing)
        return NA_STATUS_INVALID_HANDLE;

    u32 slot = 0;
    const auto reservation = mappings_.reserve(iova, memory::page_size, configuration_.address_width, slot);
    if (reservation != dma_mapping_error::none)
        return map_error_status(reservation);

    phy_addr_t frame = nullptr;
    const auto frame_status = backing->dma_page_frame(object_offset, direction, frame);
    if (frame_status != NA_STATUS_OK)
    {
        (void)mappings_.cancel(slot);
        return frame_status;
    }
    const u64 physical_address = reinterpret_cast<u64>(frame.get());
    if (!configuration_.iommu_attached && iova != physical_address)
    {
        (void)mappings_.cancel(slot);
        return NA_STATUS_ACCESS_DENIED;
    }

    bool lease_outcome_uncertain = false;
    if (!reserve_dma_window(physical_address, lease_outcome_uncertain))
    {
        (void)mappings_.cancel(slot);
        if (lease_outcome_uncertain)
        {
            state_uncertain_.store(true, std::memory_order_release);
            quarantine_backing(std::move(backing));
            return {NA_STATUS_IO_ERROR, true};
        }
        return NA_STATUS_ACCESS_DENIED;
    }

    const auto commit = mappings_.commit(slot, physical_address, direction, std::move(backing));
    if (commit != dma_mapping_error::none)
    {
        (void)mappings_.cancel(slot);
        if (!revoke_dma_window(physical_address))
        {
            state_uncertain_.store(true, std::memory_order_release);
            quarantine_backing(std::move(backing));
            return NA_STATUS_IO_ERROR;
        }
        return map_error_status(commit);
    }

    if (!configuration_.iommu_attached)
        return NA_STATUS_OK;
    if (operations_.map_page == nullptr)
    {
        handle_t<naos::data_plane::memory_object> released;
        (void)mappings_.rollback_failed_map(iova, released);
        if (!revoke_dma_window(physical_address))
        {
            state_uncertain_.store(true, std::memory_order_release);
            quarantine_backing(std::move(released));
            return NA_STATUS_IO_ERROR;
        }
        return NA_STATUS_NOT_SUPPORTED;
    }

    bool outcome_uncertain = false;
    const auto status =
        operations_.map_page(context_, backend_state_, iova, physical_address, direction, outcome_uncertain);
    if (status == NA_STATUS_OK && !outcome_uncertain)
        return NA_STATUS_OK;
    if (outcome_uncertain)
    {
        (void)mappings_.mark_uncertain(iova);
        state_uncertain_.store(true, std::memory_order_release);
        return {NA_STATUS_IO_ERROR, true};
    }

    handle_t<naos::data_plane::memory_object> released;
    const auto rollback = mappings_.rollback_failed_map(iova, released);
    if (rollback != dma_mapping_error::none)
    {
        (void)mappings_.mark_uncertain(iova);
        state_uncertain_.store(true, std::memory_order_release);
        return NA_STATUS_IO_ERROR;
    }
    if (!revoke_dma_window(physical_address))
    {
        state_uncertain_.store(true, std::memory_order_release);
        quarantine_backing(std::move(released));
        return NA_STATUS_IO_ERROR;
    }
    return status;
}

dma_operation_result dma_domain::unmap_page(process_id caller, u64 iova)
{
    if (caller != owner_)
        return NA_STATUS_ACCESS_DENIED;
    operation_guard operation(operation_in_progress_);
    if (!operation)
        return NA_STATUS_WOULD_BLOCK;
    if (closed_)
        return NA_STATUS_OBJECT_REVOKED;
    if (state_uncertain_.load(std::memory_order_acquire))
        return NA_STATUS_IO_ERROR;
    if (bus_master_enabled_)
        return NA_STATUS_ACCESS_DENIED;

    const auto *mapping = mappings_.find(iova);
    if (mapping == nullptr)
        return NA_STATUS_INVALID_ARGUMENT;
    if (mapping->state != dma_mapping_state::mapped)
        return mapping->state == dma_mapping_state::uncertain ? NA_STATUS_IO_ERROR : NA_STATUS_WOULD_BLOCK;
    const u64 physical_address = mapping->physical_address;

    if (configuration_.iommu_attached)
    {
        if (operations_.unmap_page == nullptr)
            return NA_STATUS_NOT_SUPPORTED;
        bool outcome_uncertain = false;
        const auto status = operations_.unmap_page(context_, backend_state_, iova, outcome_uncertain);
        if (status != NA_STATUS_OK || outcome_uncertain)
        {
            if (outcome_uncertain)
            {
                (void)mappings_.mark_uncertain(iova);
                state_uncertain_.store(true, std::memory_order_release);
                return {NA_STATUS_IO_ERROR, true};
            }
            return status;
        }
    }

    handle_t<naos::data_plane::memory_object> released;
    const auto release = mappings_.release_after_unmap(iova, bus_master_enabled_, released);
    if (release != dma_mapping_error::none)
        return map_error_status(release);
    if (!revoke_dma_window(physical_address))
    {
        state_uncertain_.store(true, std::memory_order_release);
        quarantine_backing(std::move(released));
        return NA_STATUS_IO_ERROR;
    }
    return NA_STATUS_OK;
}

bool dma_domain::reserve_dma_window(u64 physical_address, bool &outcome_uncertain)
{
    outcome_uncertain = false;
    constexpr u64 page_size = memory::page_size;
    if (resource_root_ == nullptr || resource_root_->resources() == nullptr || physical_address > ~u64{0} - page_size ||
        (physical_address & (page_size - 1)) != 0 || identity_.firmware_generation == 0)
        return false;

    auto *ledger = resource_root_->resources();
    physical_resource::lease lease{};
    if (!ledger->reserve(*resource_root_, physical_address, page_size, physical_address, physical_address + page_size,
                         physical_resource::kind::dma_window, physical_resource::cache::write_back,
                         physical_resource::access::read_write, physical_resource::ledger::kernel_owner(),
                         physical_resource::sharing::exclusive, identity_.firmware_generation, lease))
        return false;
    if (!ledger->transition(lease.id, physical_resource::state::reserved, physical_resource::state::leased) ||
        !ledger->map(lease.id, physical_address, page_size, physical_resource::kind::dma_window,
                     physical_resource::cache::write_back, physical_resource::access::read_write,
                     identity_.firmware_generation, physical_resource::ledger::kernel_owner()))
    {
        outcome_uncertain = !ledger->revoke_dma_window(physical_address, identity_.firmware_generation);
        return false;
    }
    return true;
}

bool dma_domain::revoke_dma_window(u64 physical_address)
{
    return resource_root_ != nullptr && resource_root_->resources() != nullptr &&
           resource_root_->resources()->revoke_dma_window(physical_address, identity_.firmware_generation);
}

dma_operation_result dma_domain::set_bus_master_locked(bool enabled)
{
    if (closed_)
        return NA_STATUS_OBJECT_REVOKED;
    if (operations_.set_bus_master == nullptr)
        return NA_STATUS_NOT_SUPPORTED;

    if (enabled)
    {
        if (state_uncertain_.load(std::memory_order_acquire))
            return NA_STATUS_IO_ERROR;
        if (bus_master_enabled_)
            return NA_STATUS_OK;
        const dma_authority::bus_master_state state{
            true,  configuration_.domain_ready, configuration_.iommu_attached, configuration_.trusted_driver_override,
            false,
        };
        if (dma_authority::bus_master_enable_denial(state) != dma_authority::bus_master_denial::none)
            return NA_STATUS_ACCESS_DENIED;

        const bus_master_enable_authority authority(identity_.segment, static_cast<u8>(identity_.bus),
                                                    static_cast<u8>(identity_.device),
                                                    static_cast<u8>(identity_.function), state);
        bool outcome_uncertain = false;
        const auto status = operations_.set_bus_master(context_, backend_state_, true, &authority, outcome_uncertain);
        if (outcome_uncertain)
            state_uncertain_.store(true, std::memory_order_release);
        if (status == NA_STATUS_OK && !outcome_uncertain)
            bus_master_enabled_ = true;
        return outcome_uncertain ? dma_operation_result{NA_STATUS_IO_ERROR, true} : dma_operation_result{status};
    }

    if (!bus_master_enabled_ && !state_uncertain_.load(std::memory_order_acquire))
        return NA_STATUS_OK;
    bool outcome_uncertain = false;
    const auto status = operations_.set_bus_master(context_, backend_state_, false, nullptr, outcome_uncertain);
    if (outcome_uncertain)
        state_uncertain_.store(true, std::memory_order_release);
    if (status == NA_STATUS_OK && !outcome_uncertain)
        bus_master_enabled_ = false;
    return outcome_uncertain ? dma_operation_result{NA_STATUS_IO_ERROR, true} : dma_operation_result{status};
}

dma_operation_result dma_domain::set_bus_master(process_id caller, bool enabled)
{
    if (caller != owner_)
        return NA_STATUS_ACCESS_DENIED;
    operation_guard operation(operation_in_progress_);
    if (!operation)
        return NA_STATUS_WOULD_BLOCK;
    return set_bus_master_locked(enabled);
}

dma_operation_result dma_domain::destroy_backend_locked()
{
    if (closed_)
        return NA_STATUS_OK;
    if (operations_.destroy == nullptr)
        return NA_STATUS_NOT_SUPPORTED;

    bool outcome_uncertain = false;
    const auto status = operations_.destroy(context_, backend_state_, outcome_uncertain);
    if (outcome_uncertain)
        state_uncertain_.store(true, std::memory_order_release);
    if (status != NA_STATUS_OK || outcome_uncertain)
        return outcome_uncertain ? dma_operation_result{NA_STATUS_IO_ERROR, true} : dma_operation_result{status};

    backend_state_ = nullptr;
    closed_ = true;
    return NA_STATUS_OK;
}

dma_operation_result dma_domain::close(process_id caller)
{
    if (caller != owner_)
        return NA_STATUS_ACCESS_DENIED;
    operation_guard operation(operation_in_progress_);
    if (!operation)
        return NA_STATUS_WOULD_BLOCK;
    if (closed_)
        return NA_STATUS_OK;
    if (state_uncertain_.load(std::memory_order_acquire))
        return NA_STATUS_IO_ERROR;
    if (bus_master_enabled_)
    {
        const auto status = set_bus_master_locked(false);
        if (status.status != NA_STATUS_OK || status.outcome_unknown)
            return status;
    }
    if (mappings_.size() != 0)
        return NA_STATUS_WOULD_BLOCK;
    return destroy_backend_locked();
}

void dma_domain::quarantine_mappings()
{
    for (u32 slot = 0; slot < max_dma_mappings_per_domain; ++slot)
    {
        auto *mapping = mappings_.at(slot);
        if (mapping != nullptr && mapping->state != dma_mapping_state::free && mapping->resource)
            quarantine_backing(std::move(mapping->resource));
    }
}

void dma_domain::teardown_noexcept()
{
    if (closed_)
        return;

    if (operations_.set_bus_master == nullptr)
    {
        state_uncertain_.store(true, std::memory_order_release);
        quarantine_mappings();
        backend_state_ = nullptr;
        closed_ = true;
        return;
    }

    bool outcome_uncertain = false;
    const auto disable_status = operations_.set_bus_master(context_, backend_state_, false, nullptr, outcome_uncertain);
    if (disable_status != NA_STATUS_OK || outcome_uncertain)
    {
        state_uncertain_.store(true, std::memory_order_release);
        quarantine_mappings();
        backend_state_ = nullptr;
        closed_ = true;
        return;
    }
    bus_master_enabled_ = false;

    if (state_uncertain_.load(std::memory_order_acquire))
    {
        quarantine_mappings();
        backend_state_ = nullptr;
        closed_ = true;
        return;
    }

    const u32 unmapped_mappings = mappings_.size();
    for (u32 slot = 0; slot < max_dma_mappings_per_domain; ++slot)
    {
        auto *mapping = mappings_.at(slot);
        if (mapping == nullptr || mapping->state == dma_mapping_state::free)
            continue;
        if (mapping->state != dma_mapping_state::mapped)
        {
            state_uncertain_.store(true, std::memory_order_release);
            quarantine_mappings();
            backend_state_ = nullptr;
            closed_ = true;
            return;
        }
        const u64 physical_address = mapping->physical_address;

        if (configuration_.iommu_attached)
        {
            bool unmap_uncertain = false;
            const auto status = operations_.unmap_page == nullptr
                                    ? NA_STATUS_NOT_SUPPORTED
                                    : operations_.unmap_page(context_, backend_state_, mapping->iova, unmap_uncertain);
            if (status != NA_STATUS_OK || unmap_uncertain)
            {
                (void)mappings_.mark_uncertain(mapping->iova);
                state_uncertain_.store(true, std::memory_order_release);
                quarantine_mappings();
                backend_state_ = nullptr;
                closed_ = true;
                return;
            }
        }

        handle_t<naos::data_plane::memory_object> released;
        if (mappings_.release_after_unmap(mapping->iova, false, released) != dma_mapping_error::none)
        {
            state_uncertain_.store(true, std::memory_order_release);
            quarantine_mappings();
            backend_state_ = nullptr;
            closed_ = true;
            return;
        }
        if (!revoke_dma_window(physical_address))
        {
            state_uncertain_.store(true, std::memory_order_release);
            quarantine_backing(std::move(released));
            quarantine_mappings();
            backend_state_ = nullptr;
            closed_ = true;
            return;
        }
    }

    if (destroy_backend_locked().status != NA_STATUS_OK)
    {
        state_uncertain_.store(true, std::memory_order_release);
        backend_state_ = nullptr;
        closed_ = true;
        return;
    }
    KLOG_INFO("PCI DMA domain teardown complete segment={} bus={} device={} function={} mappings={}", identity_.segment,
              identity_.bus, identity_.device, identity_.function, unmapped_mappings);
}

static_assert(sizeof(dma_domain) < 8192, "DmaDomain must fit the fixed kernel object slab");
} // namespace pci
