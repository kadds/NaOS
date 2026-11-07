#include "kernel/iommu/amd_vi.hpp"
#include "kernel/iommu/firmware_inventory.hpp"

namespace iommu::amd_vi
{
namespace
{
constexpr u32 device_table_base_register = 0x0000;
constexpr u32 command_buffer_base_register = 0x0008;
constexpr u32 event_log_base_register = 0x0010;
constexpr u32 control_register = 0x0018;
constexpr u32 interrupt_features_register = 0x01a0;
constexpr u32 command_head_register = 0x2000;
constexpr u32 command_tail_register = 0x2008;
constexpr u32 event_head_register = 0x2010;
constexpr u32 event_tail_register = 0x2018;
constexpr u32 status_register = 0x2020;

constexpr u64 iommu_enable = 1ull << 0;
constexpr u64 hypertransport_tunnel_enable = 1ull << 1;
constexpr u64 event_log_enable = 1ull << 2;
constexpr u64 event_log_interrupt_enable = 1ull << 3;
constexpr u64 completion_wait_interrupt_enable = 1ull << 4;
constexpr u64 pass_posted_write = 1ull << 8;
constexpr u64 response_pass_posted_write = 1ull << 9;
constexpr u64 coherent = 1ull << 10;
constexpr u64 isochronous = 1ull << 11;
constexpr u64 command_buffer_enable = 1ull << 12;
constexpr u64 command_event_enable_mask = iommu_enable | event_log_enable | command_buffer_enable;
constexpr u64 efr_invalidate_all_support = 1ull << 6;
constexpr u64 feature_report_invalidate_all_support = 1ull << 5;
constexpr u64 status_event_overflow = 1ull << 0;
constexpr u64 status_event_interrupt = 1ull << 1;
constexpr u64 status_completion_wait = 1ull << 2;
constexpr u64 status_event_log_running = 1ull << 3;
constexpr u64 status_command_buffer_running = 1ull << 4;
constexpr u64 status_running_mask = status_event_log_running | status_command_buffer_running;
constexpr u32 interrupt_remapping_support_shift = 8;
constexpr u64 interrupt_remapping_support_mask = 3ull << interrupt_remapping_support_shift;
constexpr u64 command_pointer_mask = 0x7fff0;
constexpr u32 register_poll_limit = 1'000'000;
constexpr u64 maximum_system_physical_address = (1ull << 52) - 1;
constexpr u16 default_domain_id = default_deny_domain_id;

void write_memory_barrier() { __asm__ __volatile__("mfence" : : : "memory"); }

bool physical_range_valid(u64 base, u32 length)
{
    return base != 0 && (base & (dte_page_size - 1)) == 0 && length != 0 && base <= maximum_system_physical_address &&
           static_cast<u64>(length - 1) <= maximum_system_physical_address - base;
}

bool same_device_table_entry(const device_table_entry &left, const device_table_entry &right)
{
    return left.words[0] == right.words[0] && left.words[1] == right.words[1] && left.words[2] == right.words[2] &&
           left.words[3] == right.words[3];
}

void clear_buffer(void *buffer, u32 bytes)
{
    auto *data = static_cast<u8 *>(buffer);
    for (u32 index = 0; index < bytes; ++index)
        data[index] = 0;
}

bool allocate_buffer(const controller_ops &operations, u32 bytes, void *&virtual_address, u64 &physical_address)
{
    virtual_address = nullptr;
    physical_address = 0;
    if (!operations.allocate_contiguous(operations.context, bytes, virtual_address, physical_address) ||
        virtual_address == nullptr || (reinterpret_cast<u64>(virtual_address) & (dte_page_size - 1)) != 0 ||
        !physical_range_valid(physical_address, bytes))
        return false;
    return true;
}

bool wait_for_command_head(controller_ops const &operations, unit_state &unit, u32 expected_head)
{
    for (u32 attempt = 0; attempt < register_poll_limit; ++attempt)
    {
        u64 raw_head = 0;
        if (!operations.read64(operations.context, unit.register_base, command_head_register, raw_head))
            return false;
        const u64 head = raw_head & command_pointer_mask;
        if (head >= command_buffer_bytes || (head % sizeof(command_entry)) != 0)
            return false;
        if (head == expected_head)
            return true;
    }
    return false;
}

bool submit_command_batch(const controller_ops &operations, unit_state &unit, const command_entry *commands,
                          u32 command_count)
{
    if (commands == nullptr || command_count == 0 || command_count > maximum_commands_per_batch)
        return false;
    if (!wait_for_command_head(operations, unit, unit.command_tail_offset))
        return false;

    u64 status = 0;
    if (!operations.read64(operations.context, unit.register_base, status_register, status))
        return false;
    if ((status & status_event_overflow) != 0)
        return false;
    if ((status & status_completion_wait) != 0 &&
        !operations.write64(operations.context, unit.register_base, status_register, status_completion_wait))
        return false;

    auto *ring = static_cast<command_entry *>(unit.command_buffer_virtual);
    u32 slot = unit.command_tail_offset / sizeof(command_entry);
    for (u32 index = 0; index < command_count; ++index)
    {
        ring[slot] = commands[index];
        slot = (slot + 1) % command_buffer_slots;
    }
    ring[slot] = encode_completion_wait();
    slot = (slot + 1) % command_buffer_slots;
    const u32 new_tail = slot * sizeof(command_entry);

    if (!operations.flush_range(operations.context, unit.command_buffer_virtual, command_buffer_bytes))
        return false;
    write_memory_barrier();
    unit.state_touched = true;
    if (!operations.write64(operations.context, unit.register_base, command_tail_register, new_tail))
        return false;

    bool completed = false;
    for (u32 attempt = 0; attempt < register_poll_limit; ++attempt)
    {
        if (!operations.read64(operations.context, unit.register_base, status_register, status))
            return false;
        if ((status & status_event_overflow) != 0)
            return false;
        if ((status & status_completion_wait) != 0)
        {
            completed = true;
            break;
        }
    }
    if (!completed)
        return false;
    if (!operations.write64(operations.context, unit.register_base, status_register, status_completion_wait))
        return false;
    if (!wait_for_command_head(operations, unit, new_tail))
        return false;

    unit.command_tail_offset = new_tail;
    return true;
}

void release_unit_buffers(const controller_ops &operations, unit_state &unit)
{
    if (operations.release_contiguous == nullptr)
        return;
    if (unit.event_log_virtual != nullptr)
        operations.release_contiguous(operations.context, unit.event_log_virtual, event_log_bytes);
    if (unit.command_buffer_virtual != nullptr)
        operations.release_contiguous(operations.context, unit.command_buffer_virtual, command_buffer_bytes);
    if (unit.device_table_virtual != nullptr)
        operations.release_contiguous(operations.context, unit.device_table_virtual, full_device_table_bytes);
    unit.event_log_virtual = nullptr;
    unit.command_buffer_virtual = nullptr;
    unit.device_table_virtual = nullptr;
    unit.event_log_physical = 0;
    unit.command_buffer_physical = 0;
    unit.device_table_physical = 0;
    unit.buffers_retained = false;
}

bool reserve_domain_id(u64 *bitmap, u16 &next_domain_id, u16 &domain_id)
{
    if (bitmap == nullptr)
        return false;
    u32 candidate = next_domain_id == 0 || next_domain_id >= default_domain_id ? 1 : next_domain_id;
    for (u32 attempt = 0; attempt < maximum_device_id_count - 2; ++attempt)
    {
        const u32 word = candidate / 64;
        const u64 mask = 1ull << (candidate % 64);
        if ((bitmap[word] & mask) == 0)
        {
            bitmap[word] |= mask;
            domain_id = static_cast<u16>(candidate);
            candidate++;
            if (candidate >= default_domain_id)
                candidate = 1;
            next_domain_id = static_cast<u16>(candidate);
            return true;
        }
        candidate++;
        if (candidate >= default_domain_id)
            candidate = 1;
    }
    return false;
}

void release_domain_id(u64 *bitmap, u16 domain_id)
{
    if (bitmap == nullptr || domain_id == 0 || domain_id == default_domain_id)
        return;
    bitmap[domain_id / 64] &= ~(1ull << (domain_id % 64));
}

bool pci_device_quiesced(const controller_ops &operations, const unit_state &unit, u16 device_id)
{
    if (operations.pci_bus_master_enabled == nullptr)
        return false;
    bool enabled = true;
    return operations.pci_bus_master_enabled(operations.context, unit.segment, device_id, enabled) && !enabled;
}

void mark_domain_uncertain(unit_state &unit, device_domain &domain, bool &controller_uncertain)
{
    unit.state_uncertain = true;
    domain.state_uncertain = true;
    controller_uncertain = true;
}

bool submit_domain_invalidation(const controller_ops &operations, unit_state &unit, command_entry *scratch,
                                u32 scratch_capacity, bool invalidate_dte, u16 device_id, u16 domain_id, u64 iova,
                                bool entire_domain, bool invalidate_interrupt_table = false)
{
    const u32 required_capacity = (invalidate_dte ? 1u : 0u) + (invalidate_interrupt_table ? 1u : 0u) + 1u;
    if (scratch == nullptr || scratch_capacity < required_capacity)
        return false;
    u32 count = 0;
    if (invalidate_dte)
        scratch[count++] = encode_device_table_entry_invalidation(device_id);
    if (!encode_iommu_pages_invalidation(domain_id, iova, entire_domain, scratch[count++]))
        return false;
    if (invalidate_interrupt_table)
        scratch[count++] = encode_interrupt_table_invalidation(device_id);
    return submit_command_batch(operations, unit, scratch, count);
}

bool submit_device_table_entry_invalidation(const controller_ops &operations, unit_state &unit, command_entry *scratch,
                                            u32 scratch_capacity, u16 device_id)
{
    if (scratch == nullptr || scratch_capacity < 1)
        return false;
    scratch[0] = encode_device_table_entry_invalidation(device_id);
    return submit_command_batch(operations, unit, scratch, 1);
}

bool submit_interrupt_table_invalidation(const controller_ops &operations, unit_state &unit, command_entry *scratch,
                                         u32 scratch_capacity, u16 device_id)
{
    if (scratch == nullptr || scratch_capacity < 1)
        return false;
    scratch[0] = encode_interrupt_table_invalidation(device_id);
    return submit_command_batch(operations, unit, scratch, 1);
}

bool publish_interrupt_dte_word(const controller_ops &operations, unit_state &unit,
                                device_table_entry &device_table_entry, u64 interrupt_word)
{
    // AMD 48882 §2.2.2.2 requires the interrupt-remapping portion of an active
    // DTE to be changed with a single 64-bit store. In particular, do not let
    // aggregate assignment turn this into multiple DTE word writes.
    volatile u64 *target = &device_table_entry.words[2];
    *target = interrupt_word;
    write_memory_barrier();
    unit.state_touched = true;
    return unit.coherent || operations.flush_range(operations.context, &device_table_entry.words[2], sizeof(u64));
}

controller_error translate_page_table_error(page_table_error failure)
{
    return failure == page_table_error::none ? controller_error::none : controller_error::page_table_operation_failed;
}

} // namespace

bool physical_address_width_from_iommu_capability(const amd_ivhd_record &record, pci_config_read32_callback read_config,
                                                  void *context, u8 &physical_address_width)
{
    physical_address_width = 0;
    if (read_config == nullptr || record.capability_offset > 0xfec ||
        (record.capability_offset & (alignof(u32) - 1)) != 0)
        return false;

    const u8 bus = static_cast<u8>(record.device_id >> 8);
    const u8 device = static_cast<u8>((record.device_id >> 3) & 0x1f);
    const u8 function = static_cast<u8>(record.device_id & 0x07);
    u32 identity = 0;
    u32 capability_header = 0;
    u32 misc_info = 0;
    const u16 misc_info_offset = static_cast<u16>(record.capability_offset + 0x10);
    if (!read_config(context, record.segment, bus, device, function, 0, identity) ||
        !read_config(context, record.segment, bus, device, function, record.capability_offset, capability_header) ||
        !read_config(context, record.segment, bus, device, function, misc_info_offset, misc_info))
        return false;

    constexpr u32 capability_id_mask = 0xffu;
    constexpr u32 capability_type_mask = 0x7u << 16;
    constexpr u32 capability_reserved_mask = 0xe0000000u;
    constexpr u32 iommu_capability_id = 0x0fu;
    constexpr u32 iommu_capability_type = 0x3u << 16;
    const u16 vendor_id = static_cast<u16>(identity & 0xffff);
    if (vendor_id == 0 || vendor_id == 0xffff || (capability_header & capability_id_mask) != iommu_capability_id ||
        (capability_header & capability_type_mask) != iommu_capability_type ||
        (capability_header & capability_reserved_mask) != 0)
        return false;

    const u8 reported_width = static_cast<u8>((misc_info >> 8) & 0x7f);
    if (reported_width < 12 || reported_width > 52)
        return false;
    physical_address_width = reported_width;
    return true;
}

bool controller::initialize(const firmware_inventory &inventory, const controller_ops &operations,
                            activation_preconditions preconditions, controller_error &failure)
{
    failure = controller_error::none;
    if (!preconditions.pci_bus_mastering_quiesced || !preconditions.interrupt_sources_safe)
    {
        failure = controller_error::unsafe_activation;
        return false;
    }
    if (preconditions.ioapic_source_count > max_ioapic_interrupt_sources ||
        (preconditions.ioapic_source_count != 0 && preconditions.ioapic_sources == nullptr))
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    if (attempted_ || inventory.ivrs_state != ivrs_inventory_state::valid || operations.read64 == nullptr ||
        operations.write64 == nullptr || operations.register_aperture_length == nullptr ||
        operations.allocate_contiguous == nullptr || operations.release_contiguous == nullptr ||
        operations.flush_range == nullptr ||
        (preconditions.ioapic_source_count != 0 && operations.pci_requester_present == nullptr))
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    attempted_ = true;
    operations_ = operations;
    ioapic_source_count_ = preconditions.ioapic_source_count;

    if (!inventory.amd_ivmd_records.empty())
    {
        failure = controller_error::reserved_memory_requires_domain;
        return false;
    }

    for (u32 index = 0; index < inventory.amd_ivhd_records.size(); ++index)
    {
        const auto &record = inventory.amd_ivhd_records.at(index);
        if (!record.selected)
            continue;
        if (unit_count_ == max_hardware_units)
        {
            failure = controller_error::too_many_units;
            return false;
        }
        if (record.register_base == 0 || (record.register_base & (dte_page_size - 1)) != 0)
        {
            failure = controller_error::invalid_register_aperture;
            return false;
        }

        unit_state candidate{};
        candidate.register_base = record.register_base;
        candidate.ivhd_record_index = index;
        candidate.segment = record.segment;
        candidate.ivhd_type = record.type;
        candidate.ivhd_flags = record.flags;
        candidate.coherent = (record.flags & 0x20) != 0;
        const bool extended_feature_image_valid = (inventory.ivrs_info & 1u) != 0 && record.type != 0x10;
        candidate.extended_features = extended_feature_image_valid ? record.extended_features : 0;
        candidate.invalidate_all_supported =
            extended_feature_image_valid
                ? (candidate.extended_features & efr_invalidate_all_support) != 0
                : record.type == 0x10 && (record.feature_info & feature_report_invalidate_all_support) != 0;
        candidate.register_aperture_bytes = record.register_length;
        if (candidate.register_aperture_bytes != 16 * 1024 && candidate.register_aperture_bytes != 512 * 1024)
        {
            failure = controller_error::invalid_register_aperture;
            return false;
        }
        if ((candidate.register_base & (candidate.register_aperture_bytes - 1)) != 0)
        {
            failure = controller_error::invalid_register_aperture;
            return false;
        }
        if (operations.register_aperture_length(operations.context, candidate.register_base) <
            candidate.register_aperture_bytes)
        {
            failure = controller_error::invalid_register_aperture;
            return false;
        }

        u64 control = 0;
        if (!operations.read64(operations.context, candidate.register_base, control_register, control))
        {
            failure = controller_error::register_io_failed;
            return false;
        }
        u64 interrupt_features = 0;
        if (!operations.read64(operations.context, candidate.register_base, interrupt_features_register,
                               interrupt_features))
        {
            failure = controller_error::register_io_failed;
            return false;
        }
        const u64 interrupt_remapping_support =
            (interrupt_features & interrupt_remapping_support_mask) >> interrupt_remapping_support_shift;
        // AMD 48882 §2.2.5 Table 19: 00b supports 512 basic entries and 01b
        // supports 2048. Reserved encodings withhold only interrupt remapping;
        // the independently valid DMA/default-deny unit can still start.
        candidate.interrupt_remapping_supported = interrupt_remapping_support <= 1;
        if ((control & command_event_enable_mask) != 0)
        {
            failure = controller_error::translation_pre_enabled;
            return false;
        }
        units_[unit_count_++] = candidate;
    }

    if (unit_count_ == 0)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }

    bool ioapic_sources_applied[max_ioapic_interrupt_sources]{};
    for (u32 index = 0; index < unit_count_; ++index)
    {
        auto &unit = units_[index];
        if (!allocate_buffer(operations, full_device_table_bytes, unit.device_table_virtual,
                             unit.device_table_physical) ||
            !allocate_buffer(operations, command_buffer_bytes, unit.command_buffer_virtual,
                             unit.command_buffer_physical) ||
            !allocate_buffer(operations, event_log_bytes, unit.event_log_virtual, unit.event_log_physical))
        {
            failure = controller_error::allocation_failed;
            goto release_unpublished_buffers;
        }
        if (!initialize_full_default_deny_device_table(unit.device_table_virtual, full_device_table_bytes,
                                                       default_domain_id))
        {
            failure = controller_error::invalid_device_table;
            goto release_unpublished_buffers;
        }

        if (unit.ivhd_record_index >= inventory.amd_ivhd_records.size())
        {
            failure = controller_error::invalid_inventory;
            goto release_unpublished_buffers;
        }
        const auto &ivhd_record = inventory.amd_ivhd_records.at(unit.ivhd_record_index);
        auto *device_entries = static_cast<device_table_entry *>(unit.device_table_virtual);
        for (u32 source_index = 0; source_index < preconditions.ioapic_source_count; ++source_index)
        {
            const auto &source = preconditions.ioapic_sources[source_index];
            if (source.ivhd_record_index != unit.ivhd_record_index)
                continue;
            if (source.segment != unit.segment)
            {
                failure = controller_error::invalid_inventory;
                goto release_unpublished_buffers;
            }

            bool source_matches_ivhd = false;
            for (u32 entry_index = 0; entry_index < ivhd_record.device_entry_count; ++entry_index)
            {
                const auto &entry = inventory.amd_ivhd_device_entries.at(ivhd_record.device_entry_start + entry_index);
                if (entry.type != 0x48 || entry.variety != 1 || entry.handle != source.ioapic_id)
                    continue;
                if (source_matches_ivhd || entry.source_id != source.device_id ||
                    entry.data_setting != source.dte_setting)
                {
                    failure = controller_error::invalid_inventory;
                    goto release_unpublished_buffers;
                }
                source_matches_ivhd = true;
            }
            if (!source_matches_ivhd)
            {
                failure = controller_error::invalid_inventory;
                goto release_unpublished_buffers;
            }

            bool pci_requester_present = true;
            if (!operations.pci_requester_present(operations.context, unit.segment, source.device_id,
                                                  pci_requester_present) ||
                pci_requester_present)
            {
                failure = controller_error::device_domain_conflict;
                goto release_unpublished_buffers;
            }

            device_table_entry passthrough_entry{};
            if (!is_default_deny_device_table_entry(device_entries[source.device_id], default_domain_id) ||
                !encode_ioapic_default_deny_device_table_entry(default_domain_id, source.dte_setting,
                                                               passthrough_entry))
            {
                failure = controller_error::device_domain_conflict;
                goto release_unpublished_buffers;
            }
            device_entries[source.device_id] = passthrough_entry;
            ioapic_sources_[source_index] = {index, source.device_id, source.ioapic_id, true};
            ioapic_sources_applied[source_index] = true;
        }

        clear_buffer(unit.command_buffer_virtual, command_buffer_bytes);
        clear_buffer(unit.event_log_virtual, event_log_bytes);
        if (!operations.flush_range(operations.context, unit.device_table_virtual, full_device_table_bytes) ||
            !operations.flush_range(operations.context, unit.command_buffer_virtual, command_buffer_bytes) ||
            !operations.flush_range(operations.context, unit.event_log_virtual, event_log_bytes))
        {
            failure = controller_error::cache_maintenance_unavailable;
            goto release_unpublished_buffers;
        }
    }

    for (u32 source_index = 0; source_index < preconditions.ioapic_source_count; ++source_index)
    {
        if (!ioapic_sources_applied[source_index])
        {
            failure = controller_error::invalid_inventory;
            goto release_unpublished_buffers;
        }
    }

    for (u32 index = 0; index < unit_count_; ++index)
    {
        auto &unit = units_[index];
        const u64 device_table_base = unit.device_table_physical | ((full_device_table_bytes / dte_page_size) - 1);
        const u64 command_buffer_base = unit.command_buffer_physical | (8ull << 56);
        const u64 event_log_base = unit.event_log_physical | (8ull << 56);
        // NumIntRemapMode[44:43]=00 selects the basic 512-entry IRTE format;
        // NumIntRemapSup was checked per unit before any table is published.
        const u64 command = command_buffer_enable | event_log_enable | iommu_enable | (unit.coherent ? coherent : 0) |
                            ((unit.ivhd_flags & 0x01) != 0 ? hypertransport_tunnel_enable : 0) |
                            ((unit.ivhd_flags & 0x02) != 0 ? pass_posted_write : 0) |
                            ((unit.ivhd_flags & 0x04) != 0 ? response_pass_posted_write : 0) |
                            ((unit.ivhd_flags & 0x08) != 0 ? isochronous : 0);

        unit.state_touched = true;
        hardware_touched_ = true;
        if (!operations.write64(operations.context, unit.register_base, device_table_base_register,
                                device_table_base) ||
            !operations.write64(operations.context, unit.register_base, command_buffer_base_register,
                                command_buffer_base) ||
            !operations.write64(operations.context, unit.register_base, event_log_base_register, event_log_base) ||
            !operations.write64(operations.context, unit.register_base, command_head_register, 0) ||
            !operations.write64(operations.context, unit.register_base, command_tail_register, 0) ||
            !operations.write64(operations.context, unit.register_base, event_head_register, 0) ||
            !operations.write64(operations.context, unit.register_base, event_tail_register, 0) ||
            !operations.write64(operations.context, unit.register_base, status_register,
                                status_event_overflow | status_event_interrupt | status_completion_wait) ||
            !operations.write64(operations.context, unit.register_base, control_register, command))
        {
            unit.state_uncertain = true;
            failure = controller_error::register_io_failed;
            goto retain_touched_state;
        }

        u64 control_readback = 0;
        if (!operations.read64(operations.context, unit.register_base, control_register, control_readback))
        {
            unit.state_uncertain = true;
            failure = controller_error::register_io_failed;
            goto retain_touched_state;
        }
        if ((control_readback & (iommu_enable | event_log_enable | command_buffer_enable)) !=
            (iommu_enable | event_log_enable | command_buffer_enable))
        {
            unit.state_uncertain = true;
            failure = controller_error::register_io_failed;
            goto retain_touched_state;
        }

        bool running = false;
        for (u32 attempt = 0; attempt < register_poll_limit; ++attempt)
        {
            u64 status = 0;
            if (!operations.read64(operations.context, unit.register_base, status_register, status))
            {
                unit.state_uncertain = true;
                failure = controller_error::register_io_failed;
                goto retain_touched_state;
            }
            if ((status & status_event_overflow) != 0)
            {
                unit.state_uncertain = true;
                failure = controller_error::event_log_overflow;
                goto retain_touched_state;
            }
            if ((status & status_running_mask) == status_running_mask)
            {
                running = true;
                break;
            }
        }
        if (!running)
        {
            unit.state_uncertain = true;
            failure = controller_error::hardware_timeout;
            goto retain_touched_state;
        }

        if (unit.invalidate_all_supported)
        {
            command_scratch_[0] = encode_invalidate_iommu_all();
            if (!submit_command_batch(operations, unit, command_scratch_, 1))
            {
                unit.state_uncertain = true;
                failure = controller_error::hardware_timeout;
                goto retain_touched_state;
            }
        }
        else
        {
            u32 batch_count = 0;
            for (u32 device_id = 0; device_id < maximum_device_id_count; ++device_id)
            {
                command_scratch_[batch_count++] = encode_device_table_entry_invalidation(static_cast<u16>(device_id));
                if (batch_count == maximum_commands_per_batch)
                {
                    if (!submit_command_batch(operations, unit, command_scratch_, batch_count))
                    {
                        unit.state_uncertain = true;
                        failure = controller_error::hardware_timeout;
                        goto retain_touched_state;
                    }
                    batch_count = 0;
                }
            }
            if (batch_count != 0 && !submit_command_batch(operations, unit, command_scratch_, batch_count))
            {
                unit.state_uncertain = true;
                failure = controller_error::hardware_timeout;
                goto retain_touched_state;
            }

            // INVALIDATE_DEVTAB_ENTRY does not invalidate cached I/O page
            // translations. When INVALIDATE_IOMMU_ALL is unavailable, clear
            // every DomainID before publishing any translated DTE so stale
            // translations from earlier firmware state cannot survive.
            batch_count = 0;
            for (u32 domain_id = 0; domain_id < maximum_device_id_count; ++domain_id)
            {
                if (!encode_iommu_pages_invalidation(static_cast<u16>(domain_id), 0, true,
                                                     command_scratch_[batch_count]))
                {
                    unit.state_uncertain = true;
                    failure = controller_error::invalid_command_pointer;
                    goto retain_touched_state;
                }
                batch_count++;
                if (batch_count == maximum_commands_per_batch)
                {
                    if (!submit_command_batch(operations, unit, command_scratch_, batch_count))
                    {
                        unit.state_uncertain = true;
                        failure = controller_error::hardware_timeout;
                        goto retain_touched_state;
                    }
                    batch_count = 0;
                }
            }
            if (batch_count != 0 && !submit_command_batch(operations, unit, command_scratch_, batch_count))
            {
                unit.state_uncertain = true;
                failure = controller_error::hardware_timeout;
                goto retain_touched_state;
            }
        }
        unit.default_deny_active = true;
        unit.buffers_retained = true;
        active_unit_count_++;
    }

    default_deny_ready_ = active_unit_count_ == unit_count_;
    if (!default_deny_ready_)
    {
        failure = controller_error::hardware_timeout;
        state_uncertain_ = true;
        return false;
    }
    return true;

release_unpublished_buffers:
    if (!hardware_touched_)
    {
        for (u32 index = 0; index < unit_count_; ++index)
            release_unit_buffers(operations, units_[index]);
    }
    return false;

retain_touched_state:
    state_uncertain_ = true;
    for (u32 index = 0; index < unit_count_; ++index)
    {
        auto &unit = units_[index];
        if (unit.state_touched)
            unit.buffers_retained = true;
        else
            release_unit_buffers(operations, unit);
    }
    return false;
}

void controller::quarantine_unit(u32 unit_index)
{
    if (unit_index >= unit_count_)
        return;
    units_[unit_index].state_uncertain = true;
    state_uncertain_ = true;
}

bool controller::find_unit_for_ivhd_record(u32 record_index, u32 &unit_index) const
{
    unit_index = ~u32{0};
    if (!default_deny_ready_)
        return false;
    for (u32 index = 0; index < unit_count_; ++index)
    {
        if (units_[index].ivhd_record_index != record_index)
            continue;
        if (unit_index != ~u32{0})
        {
            unit_index = ~u32{0};
            return false;
        }
        unit_index = index;
    }
    return unit_index != ~u32{0};
}

bool controller::drain_events(u32 unit_index, event_record *records, u32 capacity, event_drain_result &result,
                              controller_error &failure)
{
    result = {};
    failure = controller_error::none;
    if (!default_deny_ready_ || unit_index >= unit_count_ || (capacity != 0 && records == nullptr))
    {
        failure = controller_error::invalid_inventory;
        return false;
    }

    auto &unit = units_[unit_index];
    if (unit.event_log_virtual == nullptr || unit.event_log_overflowed || unit.state_uncertain)
    {
        failure =
            unit.event_log_overflowed ? controller_error::event_log_overflow : controller_error::invalid_inventory;
        result.overflow = unit.event_log_overflowed;
        return false;
    }

    u64 status = 0;
    u64 raw_tail = 0;
    if (!operations_.read64(operations_.context, unit.register_base, status_register, status) ||
        !operations_.read64(operations_.context, unit.register_base, event_tail_register, raw_tail))
    {
        failure = controller_error::register_io_failed;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        return false;
    }
    const u64 tail_value = raw_tail & command_pointer_mask;
    if (tail_value >= event_log_bytes || (tail_value % sizeof(command_entry)) != 0 ||
        unit.event_head_offset >= event_log_bytes || (unit.event_head_offset % sizeof(command_entry)) != 0)
    {
        failure = controller_error::invalid_event_pointer;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        return false;
    }

    const u32 tail = static_cast<u32>(tail_value);
    const u32 available_bytes = tail >= unit.event_head_offset ? tail - unit.event_head_offset
                                                               : event_log_bytes - unit.event_head_offset + tail;
    const u32 available = available_bytes / sizeof(command_entry);
    const u32 count = available < capacity ? available : capacity;
    auto *event_words = static_cast<volatile const u64 *>(unit.event_log_virtual);
    write_memory_barrier();
    u32 head = unit.event_head_offset;
    for (u32 index = 0; index < count; ++index)
    {
        const u32 slot = head / sizeof(command_entry);
        const u64 first = event_words[slot * 2];
        const u64 second = event_words[slot * 2 + 1];
        const u32 event_flags = static_cast<u32>(first >> 32);
        auto &record = records[index];
        record = {};
        record.raw[0] = first;
        record.raw[1] = second;
        record.device_id = static_cast<u16>(first);
        record.event_code = static_cast<u8>(event_flags >> 28);
        record.address = second;
        if (record.event_code == 0x02)
        {
            record.is_translation = (event_flags & (1u << 24)) != 0;
            record.is_present = (event_flags & (1u << 20)) != 0;
            record.is_write = (event_flags & (1u << 21)) != 0;
            record.is_interrupt = (event_flags & (1u << 19)) != 0;
            record.domain_or_pasid = static_cast<u16>(event_flags);
        }
        head = (head + sizeof(command_entry)) % event_log_bytes;
    }

    result.count = count;
    result.more = available > count;
    result.overflow = (status & status_event_overflow) != 0;
    unit.event_head_offset = head;
    if (count != 0)
    {
        write_memory_barrier();
        if (!operations_.write64(operations_.context, unit.register_base, event_head_register, head))
        {
            failure = controller_error::register_io_failed;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            return false;
        }
    }
    if (result.overflow)
    {
        unit.event_log_overflowed = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::event_log_overflow;
        return false;
    }
    return true;
}

bool controller::create_device_domain(u32 unit_index, u16 device_id, u8 physical_address_width,
                                      const page_table_ops &page_operations, device_domain &domain,
                                      controller_error &failure)
{
    failure = controller_error::none;
    if (!default_deny_ready_ || unit_index >= unit_count_ || domain.valid ||
        domain.translations.root_virtual != nullptr)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }

    auto &unit = units_[unit_index];
    if (!unit.default_deny_active || unit.state_uncertain)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    if (!pci_device_quiesced(operations_, unit, device_id))
    {
        failure = controller_error::device_not_quiesced;
        return false;
    }

    u16 domain_id = 0;
    if (!reserve_domain_id(allocated_domain_ids_[unit_index], next_domain_id_[unit_index], domain_id))
    {
        failure = controller_error::domain_id_exhausted;
        return false;
    }

    page_table translations{};
    const auto allocation = create_page_table(physical_address_width, unit.coherent, page_operations, translations);
    if (allocation != page_table_error::none)
    {
        release_domain_id(allocated_domain_ids_[unit_index], domain_id);
        failure = translate_page_table_error(allocation);
        return false;
    }

    auto *entries = static_cast<device_table_entry *>(unit.device_table_virtual);
    interrupt_table_state *interrupt_table = nullptr;
    for (auto &candidate : interrupt_tables_)
    {
        if (!candidate.active || candidate.unit_index != unit_index || candidate.device_id != device_id)
            continue;
        if (interrupt_table != nullptr)
        {
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::invalid_interrupt_route;
            release_domain_id(allocated_domain_ids_[unit_index], domain_id);
            if (destroy_page_table(translations) != page_table_error::none)
                domain.translations = translations;
            return false;
        }
        interrupt_table = &candidate;
    }

    device_table_entry expected_deny{};
    bool deny_entry_valid = encode_default_deny_device_table_entry(default_domain_id, expected_deny);
    if (interrupt_table != nullptr)
    {
        device_table_entry remapped_deny{};
        deny_entry_valid =
            !interrupt_table->state_uncertain && interrupt_table->virtual_address != nullptr &&
            encode_default_deny_device_table_entry(default_domain_id, expected_deny) &&
            set_device_table_interrupt_state(expected_deny, interrupt_control::remap, interrupt_table->physical_address,
                                             maximum_basic_interrupt_table_length, false, remapped_deny);
        expected_deny = remapped_deny;
    }
    if (entries == nullptr || !deny_entry_valid || !same_device_table_entry(entries[device_id], expected_deny))
    {
        if (destroy_page_table(translations) != page_table_error::none)
            domain.translations = translations;
        release_domain_id(allocated_domain_ids_[unit_index], domain_id);
        if (interrupt_table != nullptr)
        {
            interrupt_table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
        }
        failure = controller_error::device_domain_conflict;
        return false;
    }

    device_table_entry translated_entry{};
    if (!encode_dma_domain_device_table_entry(domain_id, translations.root_physical, translated_entry))
    {
        if (destroy_page_table(translations) != page_table_error::none)
            domain.translations = translations;
        release_domain_id(allocated_domain_ids_[unit_index], domain_id);
        failure = controller_error::invalid_page_table;
        return false;
    }
    if (interrupt_table != nullptr)
    {
        device_table_entry remapped_entry{};
        if (!set_device_table_interrupt_state(translated_entry, interrupt_control::remap,
                                              interrupt_table->physical_address, maximum_basic_interrupt_table_length,
                                              false, remapped_entry))
        {
            if (destroy_page_table(translations) != page_table_error::none)
                domain.translations = translations;
            release_domain_id(allocated_domain_ids_[unit_index], domain_id);
            failure = controller_error::invalid_interrupt_route;
            return false;
        }
        translated_entry = remapped_entry;
    }

    domain.translations = translations;
    domain.unit_index = unit_index;
    domain.device_id = device_id;
    domain.domain_id = domain_id;
    domain.valid = true;
    entries[device_id] = translated_entry;
    write_memory_barrier();
    unit.state_touched = true;
    if (!operations_.flush_range(operations_.context, &entries[device_id], sizeof(device_table_entry)))
    {
        mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = controller_error::cache_maintenance_unavailable;
        return false;
    }
    if (!submit_domain_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch, true, device_id,
                                    domain_id, invalidate_all_domain_pages_address, true, interrupt_table != nullptr))
    {
        mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = controller_error::hardware_timeout;
        return false;
    }
    return true;
}

bool controller::map_device_page(device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                                 controller_error &failure)
{
    failure = controller_error::none;
    if (!default_deny_ready_ || !domain.valid || domain.unit_index >= unit_count_ || domain.state_uncertain)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    auto &unit = units_[domain.unit_index];
    if (!unit.default_deny_active || unit.state_uncertain)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    const auto page_failure = map_dma_page(domain.translations, iova, physical_address, access);
    if (page_failure != page_table_error::none)
    {
        if (domain.translations.state_uncertain)
            mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = translate_page_table_error(page_failure);
        return false;
    }
    if (!submit_domain_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch, false,
                                    domain.device_id, domain.domain_id, iova, false))
    {
        mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = controller_error::hardware_timeout;
        return false;
    }
    return true;
}

bool controller::unmap_device_page(device_domain &domain, u64 iova, controller_error &failure)
{
    failure = controller_error::none;
    if (!default_deny_ready_ || !domain.valid || domain.unit_index >= unit_count_ || domain.state_uncertain)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    auto &unit = units_[domain.unit_index];
    if (!unit.default_deny_active || unit.state_uncertain)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    const auto page_failure = unmap_dma_page(domain.translations, iova);
    if (page_failure != page_table_error::none)
    {
        if (domain.translations.state_uncertain)
            mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = translate_page_table_error(page_failure);
        return false;
    }
    if (!submit_domain_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch, false,
                                    domain.device_id, domain.domain_id, iova, false))
    {
        mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = controller_error::hardware_timeout;
        return false;
    }
    return true;
}

bool controller::destroy_device_domain(device_domain &domain, controller_error &failure)
{
    failure = controller_error::none;
    if (!domain.valid)
    {
        if (domain.translations.root_virtual == nullptr)
            return true;
        const auto page_failure = destroy_page_table(domain.translations);
        if (page_failure != page_table_error::none)
        {
            failure = translate_page_table_error(page_failure);
            return false;
        }
        domain = {};
        return true;
    }
    if (!default_deny_ready_ || domain.unit_index >= unit_count_ || domain.state_uncertain || domain.domain_id == 0 ||
        domain.domain_id == default_domain_id)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }

    auto &unit = units_[domain.unit_index];
    if (!unit.default_deny_active || unit.state_uncertain)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    if (!pci_device_quiesced(operations_, unit, domain.device_id))
    {
        failure = controller_error::device_not_quiesced;
        return false;
    }

    auto *entries = static_cast<device_table_entry *>(unit.device_table_virtual);
    interrupt_table_state *interrupt_table = nullptr;
    for (auto &candidate : interrupt_tables_)
    {
        if (!candidate.active || candidate.unit_index != domain.unit_index || candidate.device_id != domain.device_id)
            continue;
        if (interrupt_table != nullptr || candidate.state_uncertain || candidate.virtual_address == nullptr)
        {
            mark_domain_uncertain(unit, domain, state_uncertain_);
            failure = controller_error::invalid_interrupt_route;
            return false;
        }
        interrupt_table = &candidate;
    }

    device_table_entry expected_entry{};
    bool expected_entry_valid =
        encode_dma_domain_device_table_entry(domain.domain_id, domain.translations.root_physical, expected_entry);
    if (expected_entry_valid && interrupt_table != nullptr)
    {
        device_table_entry remapped_entry{};
        expected_entry_valid = set_device_table_interrupt_state(
            expected_entry, interrupt_control::remap, interrupt_table->physical_address,
            maximum_basic_interrupt_table_length, false, remapped_entry);
        expected_entry = remapped_entry;
    }
    if (entries == nullptr || !expected_entry_valid ||
        !same_device_table_entry(entries[domain.device_id], expected_entry))
    {
        mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = controller_error::device_domain_conflict;
        return false;
    }

    device_table_entry deny_entry{};
    if (!encode_default_deny_device_table_entry(default_domain_id, deny_entry))
    {
        failure = controller_error::invalid_device_table;
        return false;
    }
    if (interrupt_table != nullptr)
    {
        device_table_entry remapped_deny{};
        if (!set_device_table_interrupt_state(deny_entry, interrupt_control::remap, interrupt_table->physical_address,
                                              maximum_basic_interrupt_table_length, false, remapped_deny))
        {
            failure = controller_error::invalid_interrupt_route;
            return false;
        }
        deny_entry = remapped_deny;
    }
    entries[domain.device_id] = deny_entry;
    write_memory_barrier();
    unit.state_touched = true;
    if (!operations_.flush_range(operations_.context, &entries[domain.device_id], sizeof(device_table_entry)))
    {
        mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = controller_error::cache_maintenance_unavailable;
        return false;
    }
    if (!submit_domain_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch, true,
                                    domain.device_id, domain.domain_id, invalidate_all_domain_pages_address, true,
                                    interrupt_table != nullptr))
    {
        mark_domain_uncertain(unit, domain, state_uncertain_);
        failure = controller_error::hardware_timeout;
        return false;
    }

    domain.valid = false;
    const auto page_failure = destroy_page_table(domain.translations);
    if (page_failure != page_table_error::none)
    {
        domain.state_uncertain = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = translate_page_table_error(page_failure);
        return false;
    }
    release_domain_id(allocated_domain_ids_[domain.unit_index], domain.domain_id);
    domain = {};
    return true;
}

void controller::reset_interrupt_table_state(interrupt_table_state &table)
{
    table.virtual_address = nullptr;
    table.physical_address = 0;
    table.unit_index = ~u32{0};
    table.device_id = 0;
    table.route_count = 0;
    table.active = false;
    table.state_uncertain = false;
    table.ioapic_source = false;
    for (u32 index = 0; index < interrupt_route_bitmap_words; ++index)
        table.allocated_routes[index] = 0;
    for (u32 index = 0; index < maximum_basic_interrupt_table_entries; ++index)
        table.route_cookies[index] = 0;
}

bool controller::install_pci_interrupt_route(u32 unit_index, u16 device_id, u32 destination_apic_id, u8 vector,
                                             interrupt_delivery_mode delivery, bool suppress_fault_event,
                                             interrupt_route &route, controller_error &failure)
{
    return install_interrupt_route(unit_index, device_id, false, 0xffff, destination_apic_id, vector, delivery,
                                   suppress_fault_event, route, failure);
}

bool controller::install_ioapic_interrupt_route(u32 unit_index, u16 device_id, u32 destination_apic_id, u8 vector,
                                                interrupt_route &route, controller_error &failure)
{
    // The incoming IOAPIC vector indexes the IR table. Keep it equal to the
    // allocated CPU vector so level-triggered EOI bookkeeping remains direct.
    return install_interrupt_route(unit_index, device_id, true, vector, destination_apic_id, vector,
                                   interrupt_delivery_mode::fixed, false, route, failure);
}

bool controller::is_authorized_ioapic_source(u32 unit_index, u16 device_id) const
{
    for (u32 index = 0; index < ioapic_source_count_; ++index)
    {
        const auto &source = ioapic_sources_[index];
        if (source.active && source.unit_index == unit_index && source.device_id == device_id)
            return true;
    }
    return false;
}

bool controller::install_interrupt_route(u32 unit_index, u16 device_id, bool ioapic_source, u16 requested_table_index,
                                         u32 destination_apic_id, u8 vector, interrupt_delivery_mode delivery,
                                         bool suppress_fault_event, interrupt_route &route, controller_error &failure)
{
    route = {};
    failure = controller_error::none;
    if (!default_deny_ready_ || unit_index >= unit_count_ || static_cast<u8>(delivery) > 1 ||
        next_interrupt_route_cookie_ == 0)
    {
        failure = next_interrupt_route_cookie_ == 0 ? controller_error::interrupt_table_exhausted
                                                    : controller_error::invalid_inventory;
        return false;
    }

    auto &unit = units_[unit_index];
    if (!unit.default_deny_active || unit.state_uncertain || unit.device_table_virtual == nullptr ||
        operations_.pci_requester_present == nullptr)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }
    if (!unit.interrupt_remapping_supported)
    {
        failure = controller_error::interrupt_remapping_unsupported;
        return false;
    }
    bool requester_present = false;
    if (!operations_.pci_requester_present(operations_.context, unit.segment, device_id, requester_present) ||
        (ioapic_source ? (requester_present || !is_authorized_ioapic_source(unit_index, device_id))
                       : !requester_present))
    {
        failure = controller_error::invalid_inventory;
        return false;
    }

    interrupt_table_state *table = nullptr;
    interrupt_table_state *free_table = nullptr;
    for (auto &candidate : interrupt_tables_)
    {
        if (candidate.active && candidate.unit_index == unit_index && candidate.device_id == device_id)
        {
            if (table != nullptr || candidate.ioapic_source != ioapic_source)
            {
                unit.state_uncertain = true;
                state_uncertain_ = true;
                failure = controller_error::invalid_interrupt_route;
                return false;
            }
            table = &candidate;
        }
        else if (!candidate.active && free_table == nullptr)
            free_table = &candidate;
    }

    auto *dte_entries = static_cast<device_table_entry *>(unit.device_table_virtual);
    device_table_entry current_dte = dte_entries[device_id];
    if ((current_dte.words[0] & 1u) == 0 || (current_dte.words[2] & 1u) == 0)
    {
        failure = controller_error::device_domain_conflict;
        return false;
    }

    bool attach_interrupt_table = table == nullptr;
    bool block_passthrough_ioapic = false;
    if (attach_interrupt_table)
    {
        constexpr u64 interrupt_control_mask = 3ull << 60;
        constexpr u64 interrupt_table_length_mask = 0xfull << 1;
        constexpr u64 ignore_unmapped_mask = 1ull << 5;
        const u64 current_interrupt_control = (current_dte.words[2] & interrupt_control_mask) >> 60;
        const bool expected_initial_control =
            current_interrupt_control == static_cast<u64>(interrupt_control::abort) ||
            (ioapic_source && current_interrupt_control == static_cast<u64>(interrupt_control::passthrough));
        if (!expected_initial_control ||
            (current_dte.words[2] &
             (interrupt_table_root_pointer_mask | interrupt_table_length_mask | ignore_unmapped_mask)) != 0)
        {
            failure = controller_error::device_domain_conflict;
            return false;
        }
        block_passthrough_ioapic =
            ioapic_source && current_interrupt_control == static_cast<u64>(interrupt_control::passthrough);
        if (free_table == nullptr)
        {
            failure = controller_error::interrupt_table_exhausted;
            return false;
        }

        void *table_virtual = nullptr;
        u64 table_physical = 0;
        if (!allocate_buffer(operations_, interrupt_table_bytes, table_virtual, table_physical))
        {
            failure = controller_error::allocation_failed;
            return false;
        }
        clear_buffer(table_virtual, interrupt_table_bytes);
        if (!unit.coherent && !operations_.flush_range(operations_.context, table_virtual, interrupt_table_bytes))
        {
            operations_.release_contiguous(operations_.context, table_virtual, interrupt_table_bytes);
            failure = controller_error::cache_maintenance_unavailable;
            return false;
        }
        reset_interrupt_table_state(*free_table);
        free_table->virtual_address = table_virtual;
        free_table->physical_address = table_physical;
        free_table->unit_index = unit_index;
        free_table->device_id = device_id;
        free_table->active = true;
        free_table->ioapic_source = ioapic_source;
        table = free_table;
    }
    else if (table->state_uncertain || table->virtual_address == nullptr || table->route_count == 0 ||
             table->ioapic_source != ioapic_source)
    {
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    device_table_entry expected_dte{};
    if (!set_device_table_interrupt_state(current_dte, interrupt_control::remap, table->physical_address,
                                          maximum_basic_interrupt_table_length, false, expected_dte))
    {
        if (attach_interrupt_table)
        {
            operations_.release_contiguous(operations_.context, table->virtual_address, interrupt_table_bytes);
            reset_interrupt_table_state(*table);
        }
        failure = controller_error::invalid_device_table;
        return false;
    }
    device_table_entry abort_with_table_dte{};
    if (attach_interrupt_table &&
        !set_device_table_interrupt_state(current_dte, interrupt_control::abort, table->physical_address,
                                          maximum_basic_interrupt_table_length, false, abort_with_table_dte))
    {
        operations_.release_contiguous(operations_.context, table->virtual_address, interrupt_table_bytes);
        reset_interrupt_table_state(*table);
        failure = controller_error::invalid_device_table;
        return false;
    }
    if (!attach_interrupt_table && !same_device_table_entry(current_dte, expected_dte))
    {
        table->state_uncertain = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::device_domain_conflict;
        return false;
    }

    u16 table_index = requested_table_index;
    if (table_index != 0xffff && table_index >= maximum_basic_interrupt_table_entries)
        table_index = 0xffff;
    if (table_index == 0xffff)
    {
        for (u32 index = 0; index < maximum_basic_interrupt_table_entries; ++index)
        {
            if ((table->allocated_routes[index / 64] & (1ull << (index % 64))) == 0)
            {
                table_index = static_cast<u16>(index);
                break;
            }
        }
    }
    else if ((table->allocated_routes[table_index / 64] & (1ull << (table_index % 64))) != 0)
        table_index = 0xffff;
    if (table_index == 0xffff || table->route_count == maximum_basic_interrupt_table_entries)
    {
        if (attach_interrupt_table)
        {
            operations_.release_contiguous(operations_.context, table->virtual_address, interrupt_table_bytes);
            reset_interrupt_table_state(*table);
        }
        failure = controller_error::interrupt_table_exhausted;
        return false;
    }

    interrupt_remap_entry encoded_entry{};
    if (!encode_interrupt_remap_entry(destination_apic_id, vector, delivery, suppress_fault_event, encoded_entry))
    {
        if (attach_interrupt_table)
        {
            operations_.release_contiguous(operations_.context, table->virtual_address, interrupt_table_bytes);
            reset_interrupt_table_state(*table);
        }
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    const u32 cookie = next_interrupt_route_cookie_++;
    table->allocated_routes[table_index / 64] |= 1ull << (table_index % 64);
    table->route_cookies[table_index] = cookie;
    table->route_count++;
    auto *entries = static_cast<interrupt_remap_entry *>(table->virtual_address);
    entries[table_index] = encoded_entry;
    write_memory_barrier();
    if (!unit.coherent &&
        !operations_.flush_range(operations_.context, &entries[table_index], sizeof(entries[table_index])))
    {
        if (attach_interrupt_table)
        {
            table->allocated_routes[table_index / 64] &= ~(1ull << (table_index % 64));
            table->route_cookies[table_index] = 0;
            table->route_count = 0;
            operations_.release_contiguous(operations_.context, table->virtual_address, interrupt_table_bytes);
            reset_interrupt_table_state(*table);
        }
        else
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
        }
        failure = controller_error::cache_maintenance_unavailable;
        return false;
    }

    if (attach_interrupt_table)
    {
        // Keep IntCtl=abort while publishing the new table pointer. Invalidate
        // the interrupt table before changing IntCtl to remap, then invalidate
        // the DTE so the IOMMU observes the final state.
        if (block_passthrough_ioapic)
        {
            device_table_entry blocked_dte{};
            if (!set_device_table_interrupt_state(current_dte, interrupt_control::abort, 0, 0, false, blocked_dte) ||
                !publish_interrupt_dte_word(operations_, unit, dte_entries[device_id], blocked_dte.words[2]))
            {
                table->state_uncertain = true;
                unit.state_uncertain = true;
                state_uncertain_ = true;
                failure = controller_error::cache_maintenance_unavailable;
                return false;
            }
            if (!submit_device_table_entry_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                                        device_id))
            {
                table->state_uncertain = true;
                unit.state_uncertain = true;
                state_uncertain_ = true;
                failure = controller_error::hardware_timeout;
                return false;
            }
        }
        if (!publish_interrupt_dte_word(operations_, unit, dte_entries[device_id], abort_with_table_dte.words[2]))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::cache_maintenance_unavailable;
            return false;
        }
        if (!submit_interrupt_table_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                                 device_id))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::hardware_timeout;
            return false;
        }
        if (!publish_interrupt_dte_word(operations_, unit, dte_entries[device_id], expected_dte.words[2]))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::cache_maintenance_unavailable;
            return false;
        }
        if (!submit_device_table_entry_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                                    device_id))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::hardware_timeout;
            return false;
        }
    }
    else if (!submit_interrupt_table_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                                  device_id))
    {
        table->state_uncertain = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::hardware_timeout;
        return false;
    }

    route.unit_index = unit_index;
    route.device_id = device_id;
    route.table_index = table_index;
    route.cookie = cookie;
    route.ioapic_source = ioapic_source;
    return true;
}

bool controller::retire_interrupt_route(const interrupt_route &route, controller_error &failure)
{
    failure = controller_error::none;
    if (!default_deny_ready_ || !route.valid() || route.unit_index >= unit_count_)
    {
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    auto &unit = units_[route.unit_index];
    if (!unit.default_deny_active || unit.state_uncertain)
    {
        failure = controller_error::invalid_inventory;
        return false;
    }

    interrupt_table_state *table = nullptr;
    for (auto &candidate : interrupt_tables_)
    {
        if (!candidate.active || candidate.unit_index != route.unit_index || candidate.device_id != route.device_id)
            continue;
        if (table != nullptr)
        {
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::invalid_interrupt_route;
            return false;
        }
        table = &candidate;
    }
    const u64 route_bit = 1ull << (route.table_index % 64);
    if (table == nullptr || table->ioapic_source != route.ioapic_source || table->state_uncertain ||
        table->virtual_address == nullptr || table->route_count == 0 ||
        table->route_cookies[route.table_index] != route.cookie ||
        (table->allocated_routes[route.table_index / 64] & route_bit) == 0)
    {
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    auto *dte_entries = static_cast<device_table_entry *>(unit.device_table_virtual);
    if (dte_entries == nullptr)
    {
        table->state_uncertain = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::invalid_device_table;
        return false;
    }
    const device_table_entry current_dte = dte_entries[route.device_id];
    device_table_entry expected_remapped_dte{};
    if (!set_device_table_interrupt_state(current_dte, interrupt_control::remap, table->physical_address,
                                          maximum_basic_interrupt_table_length, false, expected_remapped_dte) ||
        !same_device_table_entry(current_dte, expected_remapped_dte))
    {
        table->state_uncertain = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::device_domain_conflict;
        return false;
    }

    auto *entries = static_cast<interrupt_remap_entry *>(table->virtual_address);
    const interrupt_remap_entry current_entry = entries[route.table_index];
    if ((current_entry.value & 1u) == 0)
    {
        table->state_uncertain = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    const bool last_route = table->route_count == 1;
    device_table_entry blocked_dte{};
    if (last_route)
    {
        if (!set_device_table_interrupt_state(current_dte, interrupt_control::abort, table->physical_address,
                                              maximum_basic_interrupt_table_length, false, blocked_dte) ||
            !publish_interrupt_dte_word(operations_, unit, dte_entries[route.device_id], blocked_dte.words[2]))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::cache_maintenance_unavailable;
            return false;
        }
        if (!submit_device_table_entry_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                                    route.device_id))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::hardware_timeout;
            return false;
        }
    }

    entries[route.table_index] = {};
    write_memory_barrier();
    if (!unit.coherent &&
        !operations_.flush_range(operations_.context, &entries[route.table_index], sizeof(entries[0])))
    {
        table->state_uncertain = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::cache_maintenance_unavailable;
        return false;
    }

    if (!submit_interrupt_table_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                             route.device_id))
    {
        table->state_uncertain = true;
        unit.state_uncertain = true;
        state_uncertain_ = true;
        failure = controller_error::hardware_timeout;
        return false;
    }

    if (last_route)
    {
        device_table_entry detached_dte{};
        if (!set_device_table_interrupt_state(blocked_dte, interrupt_control::abort, 0, 0, false, detached_dte))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::invalid_device_table;
            return false;
        }
        if (!publish_interrupt_dte_word(operations_, unit, dte_entries[route.device_id], detached_dte.words[2]))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::cache_maintenance_unavailable;
            return false;
        }
        if (!submit_interrupt_table_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                                 route.device_id) ||
            !submit_device_table_entry_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                                    route.device_id))
        {
            table->state_uncertain = true;
            unit.state_uncertain = true;
            state_uncertain_ = true;
            failure = controller_error::hardware_timeout;
            return false;
        }
        if (route.ioapic_source)
        {
            device_table_entry passthrough_dte{};
            if (!set_device_table_interrupt_state(detached_dte, interrupt_control::passthrough, 0, 0, false,
                                                  passthrough_dte) ||
                !publish_interrupt_dte_word(operations_, unit, dte_entries[route.device_id], passthrough_dte.words[2]))
            {
                table->state_uncertain = true;
                unit.state_uncertain = true;
                state_uncertain_ = true;
                failure = controller_error::cache_maintenance_unavailable;
                return false;
            }
            if (!submit_device_table_entry_invalidation(operations_, unit, command_scratch_, maximum_commands_per_batch,
                                                        route.device_id))
            {
                table->state_uncertain = true;
                unit.state_uncertain = true;
                state_uncertain_ = true;
                failure = controller_error::hardware_timeout;
                return false;
            }
        }
    }

    table->allocated_routes[route.table_index / 64] &= ~route_bit;
    table->route_cookies[route.table_index] = 0;
    table->route_count--;
    if (last_route)
    {
        operations_.release_contiguous(operations_.context, table->virtual_address, interrupt_table_bytes);
        reset_interrupt_table_state(*table);
    }
    return true;
}

bool controller::pci_interrupt_route_matches(const interrupt_route &route, u32 unit_index, u16 device_id) const
{
    if (!default_deny_ready_ || !route.valid() || route.ioapic_source || route.unit_index != unit_index ||
        route.device_id != device_id || unit_index >= unit_count_ || units_[unit_index].state_uncertain)
        return false;
    for (const auto &table : interrupt_tables_)
    {
        if (!table.active || table.unit_index != unit_index || table.device_id != device_id || table.state_uncertain ||
            table.ioapic_source || table.virtual_address == nullptr ||
            table.route_cookies[route.table_index] != route.cookie)
            continue;
        if ((table.allocated_routes[route.table_index / 64] & (1ull << (route.table_index % 64))) == 0)
            return false;
        const auto *dte_entries = static_cast<const device_table_entry *>(units_[unit_index].device_table_virtual);
        if (dte_entries == nullptr)
            return false;
        const auto &current_dte = dte_entries[device_id];
        device_table_entry expected_dte{};
        if (!set_device_table_interrupt_state(current_dte, interrupt_control::remap, table.physical_address,
                                              maximum_basic_interrupt_table_length, false, expected_dte) ||
            !same_device_table_entry(current_dte, expected_dte))
            return false;
        const auto *entries = static_cast<const interrupt_remap_entry *>(table.virtual_address);
        return (entries[route.table_index].value & 1u) != 0;
    }
    return false;
}

na_status_t to_status(controller_error failure) noexcept
{
    switch (failure)
    {
        case controller_error::none:
            return NA_STATUS_OK;
        case controller_error::allocation_failed:
        case controller_error::too_many_units:
        case controller_error::domain_id_exhausted:
        case controller_error::interrupt_table_exhausted:
            return NA_STATUS_RESOURCE_EXHAUSTED;
        case controller_error::unsafe_activation:
        case controller_error::reserved_memory_requires_domain:
        case controller_error::invalid_register_aperture:
        case controller_error::translation_pre_enabled:
        case controller_error::cache_maintenance_unavailable:
        case controller_error::interrupt_remapping_unsupported:
            return NA_STATUS_NOT_SUPPORTED;
        case controller_error::invalid_inventory:
        case controller_error::invalid_device_table:
        case controller_error::device_domain_conflict:
        case controller_error::invalid_page_table:
        case controller_error::page_table_operation_failed:
        case controller_error::invalid_interrupt_route:
            return NA_STATUS_INVALID_ARGUMENT;
        case controller_error::device_not_quiesced:
            return NA_STATUS_ACCESS_DENIED;
        case controller_error::register_io_failed:
        case controller_error::hardware_timeout:
        case controller_error::invalid_command_pointer:
        case controller_error::invalid_event_pointer:
        case controller_error::event_log_overflow:
            return NA_STATUS_IO_ERROR;
    }
    return NA_STATUS_IO_ERROR;
}

interrupt_route_token encode_route_token(const interrupt_route &route, u64 generation) noexcept
{
    interrupt_route_token token{};
    token.provider = provider_kind::amd_vi;
    token.generation = generation;
    token.opaque[0] = (static_cast<u64>(route.unit_index) << 32) | route.device_id;
    token.opaque[1] = (static_cast<u64>(route.cookie) << 16) | route.table_index;
    token.opaque[1] |= route.ioapic_source ? 1ull << 48 : 0;
    return token;
}

bool decode_route_token(const interrupt_route_token &token, u64 expected_generation, interrupt_route &route) noexcept
{
    route = {};
    if (!token.valid() || token.provider != provider_kind::amd_vi || token.generation != expected_generation ||
        (token.opaque[0] >> 48) != 0 || (token.opaque[1] >> 49) != 0)
        return false;
    route.unit_index = static_cast<u32>(token.opaque[0] >> 32);
    route.device_id = static_cast<u16>(token.opaque[0]);
    route.table_index = static_cast<u16>(token.opaque[1]);
    route.cookie = static_cast<u32>(token.opaque[1] >> 16);
    route.ioapic_source = ((token.opaque[1] >> 48) & 1) != 0;
    return route.valid();
}

bool install_pci_msi_route(controller &backend, const device_source_ref &source, u64 firmware_generation,
                           u32 destination_apic_id, u8 vector, interrupt_route_token &token, msi_message &message,
                           bool &outcome_uncertain, controller_error &failure)
{
    token = {};
    message = {};
    outcome_uncertain = false;
    failure = controller_error::none;
    if (!source.valid() || source.unit.provider != provider_kind::amd_vi ||
        source.unit.generation != firmware_generation || source.unit.opaque[0] > ~u32{0} ||
        source.unit.opaque[1] > ~u32{0})
    {
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    const u32 unit_index = static_cast<u32>(source.unit.opaque[1]);
    const auto *unit = backend.unit(unit_index);
    if (unit == nullptr || unit->segment != source.segment || unit->ivhd_record_index != source.unit.opaque[0])
    {
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    interrupt_route route{};
    bool installed = backend.install_pci_interrupt_route(unit_index, source.device_id, destination_apic_id, vector,
                                                         interrupt_delivery_mode::fixed, false, route, failure);
    unit = backend.unit(unit_index);
    outcome_uncertain = unit != nullptr && unit->state_uncertain;
    if (!installed)
        return false;

    interrupt_message_encoding encoding{};
    if (!encode_interrupt_table_offset_message(route.table_index, encoding))
    {
        failure = controller_error::invalid_interrupt_route;
        controller_error cleanup_failure = controller_error::none;
        const bool retired = backend.retire_interrupt_route(route, cleanup_failure);
        if (!retired)
            failure = cleanup_failure;
        unit = backend.unit(unit_index);
        outcome_uncertain = !retired || (unit != nullptr && unit->state_uncertain);
        return false;
    }

    message = {encoding.address, encoding.data};
    token = encode_route_token(route, source.unit.generation);
    return true;
}

bool install_ioapic_route(controller &backend, const device_source_ref &source, u64 firmware_generation,
                          u32 destination_apic_id, u8 vector, bool low_polarity, bool level_triggered,
                          ioapic_route_encoding &route, controller_error &failure)
{
    route = {};
    failure = controller_error::none;
    if (!source.valid() || source.unit.provider != provider_kind::amd_vi ||
        source.unit.generation != firmware_generation || source.unit.opaque[0] > ~u32{0} ||
        source.unit.opaque[1] > ~u32{0} || destination_apic_id > 0xff)
    {
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    const u32 unit_index = static_cast<u32>(source.unit.opaque[1]);
    const auto *unit = backend.unit(unit_index);
    if (unit == nullptr || unit->segment != source.segment || unit->ivhd_record_index != source.unit.opaque[0])
    {
        failure = controller_error::invalid_interrupt_route;
        return false;
    }

    interrupt_route interrupt_route{};
    if (!backend.install_ioapic_interrupt_route(unit_index, source.device_id, destination_apic_id, vector,
                                                interrupt_route, failure))
        return false;

    // AMD 48882 §2.2.5 uses the incoming interrupt vector to index the IRTE.
    // The controller keeps that index equal to the allocated CPU vector.
    u64 redirection_entry = vector | (1ull << 16) | (static_cast<u64>(destination_apic_id) << 56);
    if (low_polarity)
        redirection_entry |= 1ull << 13;
    if (level_triggered)
        redirection_entry |= 1ull << 15;
    route.token = encode_route_token(interrupt_route, source.unit.generation);
    route.redirection_entry = redirection_entry;
    return true;
}

bool route_matches_pci(const controller &backend, const interrupt_route_token &token,
                       const device_source_ref &source) noexcept
{
    if (!source.valid() || source.unit.provider != provider_kind::amd_vi || source.unit.opaque[0] > ~u32{0} ||
        source.unit.opaque[1] > ~u32{0})
        return false;
    interrupt_route route{};
    if (!decode_route_token(token, source.unit.generation, route) || route.unit_index != source.unit.opaque[1] ||
        route.device_id != source.device_id)
        return false;
    const auto *unit = backend.unit(route.unit_index);
    return unit != nullptr && unit->segment == source.segment && unit->ivhd_record_index == source.unit.opaque[0] &&
           backend.pci_interrupt_route_matches(route, route.unit_index, source.device_id);
}

bool retire_route(controller &backend, const interrupt_route_token &token, u64 firmware_generation,
                  controller_error &failure)
{
    failure = controller_error::none;
    interrupt_route route{};
    if (!decode_route_token(token, firmware_generation, route))
    {
        failure = controller_error::invalid_interrupt_route;
        return false;
    }
    return backend.retire_interrupt_route(route, failure);
}

na_status_t map_dma_page(controller &backend, device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                         bool &outcome_uncertain)
{
    controller_error failure = controller_error::none;
    const bool mapped = backend.map_device_page(domain, iova, physical_address, access, failure);
    const auto *unit = backend.unit(domain.unit_index);
    outcome_uncertain = domain.state_uncertain || (unit != nullptr && unit->state_uncertain);
    return mapped ? NA_STATUS_OK : to_status(failure);
}

na_status_t unmap_dma_page(controller &backend, device_domain &domain, u64 iova, bool &outcome_uncertain)
{
    controller_error failure = controller_error::none;
    const bool unmapped = backend.unmap_device_page(domain, iova, failure);
    const auto *unit = backend.unit(domain.unit_index);
    outcome_uncertain = domain.state_uncertain || (unit != nullptr && unit->state_uncertain);
    return unmapped ? NA_STATUS_OK : to_status(failure);
}

na_status_t destroy_dma_domain(controller &backend, device_domain &domain, bool &outcome_uncertain)
{
    controller_error failure = controller_error::none;
    const bool destroyed = backend.destroy_device_domain(domain, failure);
    const auto *unit = backend.unit(domain.unit_index);
    outcome_uncertain = domain.state_uncertain || (unit != nullptr && unit->state_uncertain);
    return destroyed ? NA_STATUS_OK : to_status(failure);
}

resolution_result resolve_pci_source(const firmware_inventory &inventory, const controller &backend,
                                     const pci_function_location &function, u64 generation, device_source_ref &source)
{
    source = {};
    if (generation != inventory.firmware_generation)
        return resolution_result::reject;
    if (inventory.ivrs_state == ivrs_inventory_state::absent)
        return resolution_result::not_applicable;
    if (inventory.ivrs_state != ivrs_inventory_state::valid || !backend.default_deny_ready())
        return resolution_result::reject;

    u32 ivhd_record_index = ~u32{0};
    u16 requester_id = 0;
    source_resolution_error failure = source_resolution_error::none;
    if (!resolve_pci_device_unit(inventory, function.segment, function.bus, function.device, function.function,
                                 ivhd_record_index, requester_id, failure))
        return failure == source_resolution_error::device_not_found ? resolution_result::not_applicable
                                                                    : resolution_result::reject;

    u32 compact_unit_index = ~u32{0};
    if (!backend.find_unit_for_ivhd_record(ivhd_record_index, compact_unit_index))
        return resolution_result::reject;

    source.unit.provider = provider_kind::amd_vi;
    source.unit.generation = generation;
    source.unit.opaque[0] = ivhd_record_index;
    source.unit.opaque[1] = compact_unit_index;
    source.segment = function.segment;
    source.device_id = requester_id;
    return resolution_result::resolved;
}

resolution_result resolve_ioapic_requester(const firmware_inventory &inventory, u8 enumeration_id, u32 madt_match_count,
                                           u32 &ivhd_record_index, u16 &segment, u16 &requester_id, u8 &dte_setting)
{
    ivhd_record_index = ~u32{0};
    segment = 0;
    requester_id = 0;
    dte_setting = 0;
    if (inventory.ivrs_state == ivrs_inventory_state::absent)
        return resolution_result::not_applicable;
    if (inventory.ivrs_state != ivrs_inventory_state::valid)
        return resolution_result::reject;
    if (madt_match_count == 0)
        return resolution_result::not_applicable;
    if (madt_match_count != 1)
        return resolution_result::reject;

    source_resolution_error failure = source_resolution_error::none;
    if (!resolve_ioapic_source(inventory, enumeration_id, ivhd_record_index, segment, requester_id, dte_setting,
                               failure))
        return failure == source_resolution_error::device_not_found ? resolution_result::not_applicable
                                                                    : resolution_result::reject;
    return resolution_result::resolved;
}

resolution_result resolve_ioapic_source(const firmware_inventory &inventory, const controller &backend,
                                        u8 enumeration_id, u32 madt_match_count, u64 generation, u8 &dte_setting,
                                        device_source_ref &source)
{
    source = {};
    if (generation != inventory.firmware_generation || !backend.default_deny_ready())
        return resolution_result::reject;

    u32 ivhd_record_index = ~u32{0};
    u16 segment = 0;
    u16 requester_id = 0;
    const auto resolution = resolve_ioapic_requester(inventory, enumeration_id, madt_match_count, ivhd_record_index,
                                                     segment, requester_id, dte_setting);
    if (resolution != resolution_result::resolved)
        return resolution;

    u32 compact_unit_index = ~u32{0};
    if (!backend.find_unit_for_ivhd_record(ivhd_record_index, compact_unit_index))
        return resolution_result::reject;
    const auto *unit = backend.unit(compact_unit_index);
    if (unit == nullptr || unit->segment != segment || unit->ivhd_record_index != ivhd_record_index)
        return resolution_result::reject;

    source.unit.provider = provider_kind::amd_vi;
    source.unit.generation = generation;
    source.unit.opaque[0] = ivhd_record_index;
    source.unit.opaque[1] = compact_unit_index;
    source.segment = segment;
    source.device_id = requester_id;
    return resolution_result::resolved;
}
} // namespace iommu::amd_vi
