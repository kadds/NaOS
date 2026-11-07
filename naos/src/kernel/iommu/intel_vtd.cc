#include "kernel/iommu/intel_vtd.hpp"
#include "kernel/iommu/firmware_inventory.hpp"
#include "kernel/iommu/intel_vtd_interrupt.hpp"

namespace iommu::intel_vtd
{
namespace
{
constexpr u32 version_register = 0x00;
constexpr u32 capability_register = 0x08;
constexpr u32 extended_capability_register = 0x10;
constexpr u32 fault_status_register = 0x34;
constexpr u32 fault_event_control_register = 0x38;
constexpr u32 fault_event_data_register = 0x3c;
constexpr u32 fault_event_address_register = 0x40;
constexpr u32 fault_event_upper_address_register = 0x44;
constexpr u32 global_command_register = 0x18;
constexpr u32 global_status_register = 0x1c;
constexpr u32 root_table_address_register = 0x20;
constexpr u32 context_command_register = 0x28;
constexpr u32 queued_invalidation_head_register = 0x80;
constexpr u32 queued_invalidation_tail_register = 0x88;
constexpr u32 queued_invalidation_address_register = 0x90;
constexpr u32 interrupt_remapping_table_address_register = 0xb8;

constexpr u32 command_translation_enable = 1u << 31;
constexpr u32 command_set_root_table_pointer = 1u << 30;
constexpr u32 command_set_interrupt_remapping_table_pointer = 1u << 24;
constexpr u32 command_interrupt_remapping_enable = 1u << 25;
constexpr u32 command_queued_invalidation_enable = 1u << 26;
constexpr u32 status_translation_enable = 1u << 31;
constexpr u32 status_root_table_pointer = 1u << 30;
constexpr u32 status_interrupt_remapping_table_pointer = 1u << 24;
constexpr u32 status_queued_invalidation_enable = 1u << 26;
constexpr u32 status_interrupt_remapping_enable = 1u << 25;
constexpr u64 extended_capability_queued_invalidation = 1ull << 1;
constexpr u64 extended_capability_interrupt_remapping = 1ull << 3;
constexpr u32 fault_status_overflow = 1u << 0;
constexpr u32 fault_status_pending = 1u << 1;
constexpr u32 fault_status_record_index_mask = 0xffu << 8;
constexpr u32 fault_status_clear_mask = fault_status_overflow | fault_status_pending;
constexpr u32 fault_event_interrupt_mask = 1u << 31;
constexpr u32 fault_record_valid = 1u << 31;
constexpr u32 fault_record_bytes = 16;

constexpr u64 context_command_invalidate = (1ull << 63) | (1ull << 61);
constexpr u64 context_command_in_progress = 1ull << 63;
constexpr u64 iotlb_command_invalidate = (1ull << 63) | (1ull << 60);
constexpr u64 iotlb_command_in_progress = 1ull << 63;

constexpr u64 page_bytes = 4096;
constexpr u64 page_mask = page_bytes - 1;
constexpr u64 qi_register_offset_mask = 0x7fff0;
constexpr u32 qi_descriptor_bytes = 16;
constexpr u64 qi_context_cache_invalidation_type = 1;
constexpr u64 qi_iotlb_invalidation_type = 2;
constexpr u64 qi_interrupt_entry_cache_invalidation_type = 4;
constexpr u64 qi_invalidation_wait_type = 5;
constexpr u64 qi_global_invalidation_granularity = 1ull << 4;
constexpr u64 qi_invalidation_wait_status_write = 1ull << 5;
constexpr u64 qi_invalidation_wait_fence = 1ull << 6;
constexpr u32 interrupt_remapping_table_size_encoding = 7;
constexpr u64 root_entries = page_bytes / sizeof(u64);
constexpr u64 context_present = 1ull;
constexpr u64 context_pointer_mask = ~page_mask;
constexpr u32 context_domain_id_shift = 8;
constexpr u64 context_domain_id_mask = 0xffffull << context_domain_id_shift;
constexpr u32 max_register_polls = 1'000'000;

struct translation_entry
{
    u64 low;
    u64 high;
};
static_assert(sizeof(translation_entry) == 16);

struct queued_invalidation_descriptor
{
    u64 low;
    u64 high;
};
static_assert(sizeof(queued_invalidation_descriptor) == qi_descriptor_bytes);

enum class poll_result : u8
{
    complete,
    io_failure,
    timeout,
};

bool address_fits_width(u64 address, u32 width) { return width >= 64 || (address >> width) == 0; }

void write_memory_barrier() { __asm__ __volatile__("mfence" : : : "memory"); }

poll_result wait_status32(const register_ops &operations, u64 base, u32 mask, bool set)
{
    for (u32 attempt = 0; attempt < max_register_polls; ++attempt)
    {
        u32 status = 0;
        if (!operations.read32(operations.context, base, global_status_register, status))
            return poll_result::io_failure;
        if (((status & mask) != 0) == set)
            return poll_result::complete;
    }
    return poll_result::timeout;
}

poll_result wait_register64_clear(const register_ops &operations, u64 base, u32 offset, u64 mask)
{
    for (u32 attempt = 0; attempt < max_register_polls; ++attempt)
    {
        u64 value = 0;
        if (!operations.read64(operations.context, base, offset, value))
            return poll_result::io_failure;
        if ((value & mask) == 0)
            return poll_result::complete;
    }
    return poll_result::timeout;
}

bool address_range_fits_width(u64 address, u64 length, u32 width)
{
    if (length == 0 || width == 0 || address > ~u64(0) - (length - 1))
        return false;
    if (width >= 64)
        return true;
    const u64 maximum_address = (1ull << width) - 1;
    return address <= maximum_address && length - 1 <= maximum_address - address;
}

u8 adjusted_width_encoding(u8 width)
{
    switch (width)
    {
        case 30:
            return 0;
        case 39:
            return 1;
        case 48:
            return 2;
        case 57:
            return 3;
        default:
            return 0xff;
    }
}

error translate_page_table_error(page_table_error failure)
{
    switch (failure)
    {
        case page_table_error::none:
            return error::none;
        case page_table_error::already_mapped:
            return error::dma_page_already_mapped;
        case page_table_error::not_mapped:
            return error::dma_page_not_mapped;
        case page_table_error::allocation_failed:
            return error::allocation_failed;
        case page_table_error::cache_maintenance_unavailable:
            return error::cache_maintenance_unavailable;
        case page_table_error::table_state_uncertain:
            return error::domain_state_uncertain;
        case page_table_error::unsupported_address_width:
            return error::unsupported_page_table_width;
        default:
            return error::invalid_page_table;
    }
}

bool flush_table_page(const unit_state &state, void *page)
{
    return state.coherent || state.operations.flush_page(state.operations.context, page);
}

void clear_table_page(void *page)
{
    auto *entries = static_cast<u64 *>(page);
    for (u64 index = 0; index < page_bytes / sizeof(u64); ++index)
        entries[index] = 0;
}

bool submit_queued_invalidation(unit_state &state, const queued_invalidation_descriptor &descriptor, error &failure);

bool invalidate_device_translations(unit_state &state, error &failure)
{
    if (state.queued_invalidation_enabled)
    {
        const queued_invalidation_descriptor context_invalidation{
            qi_context_cache_invalidation_type | qi_global_invalidation_granularity, 0};
        if (!submit_queued_invalidation(state, context_invalidation, failure))
        {
            state.state_uncertain = true;
            return false;
        }

        const queued_invalidation_descriptor iotlb_invalidation{
            qi_iotlb_invalidation_type | qi_global_invalidation_granularity, 0};
        if (!submit_queued_invalidation(state, iotlb_invalidation, failure))
        {
            state.state_uncertain = true;
            return false;
        }
        failure = error::none;
        return true;
    }

    if (!state.operations.write64(state.operations.context, state.register_base, context_command_register,
                                  context_command_invalidate))
    {
        state.state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto context_poll = wait_register64_clear(state.operations, state.register_base, context_command_register,
                                                    context_command_in_progress);
    if (context_poll != poll_result::complete)
    {
        state.state_uncertain = true;
        failure = context_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }

    if (!state.operations.write64(state.operations.context, state.register_base, state.iotlb_register_offset,
                                  iotlb_command_invalidate))
    {
        state.state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto iotlb_poll = wait_register64_clear(state.operations, state.register_base, state.iotlb_register_offset,
                                                  iotlb_command_in_progress);
    if (iotlb_poll != poll_result::complete)
    {
        state.state_uncertain = true;
        failure = iotlb_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }
    failure = error::none;
    return true;
}

bool submit_queued_invalidation(unit_state &state, const queued_invalidation_descriptor &descriptor, error &failure)
{
    failure = error::none;
    if (!state.queued_invalidation_enabled || state.queued_invalidation_state_uncertain ||
        !state.queued_invalidation_storage_retained || state.queued_invalidation_queue_virtual == nullptr ||
        state.queued_invalidation_status_virtual == nullptr || state.operations.read64 == nullptr ||
        state.operations.write64 == nullptr)
    {
        failure = error::domain_state_uncertain;
        return false;
    }

    u64 head_register = 0;
    u64 tail_register = 0;
    if (!state.operations.read64(state.operations.context, state.register_base, queued_invalidation_head_register,
                                 head_register) ||
        !state.operations.read64(state.operations.context, state.register_base, queued_invalidation_tail_register,
                                 tail_register))
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }

    const u64 head = head_register & qi_register_offset_mask;
    const u64 tail = tail_register & qi_register_offset_mask;
    if ((head_register & ~qi_register_offset_mask) != 0 || (tail_register & ~qi_register_offset_mask) != 0 ||
        head >= page_bytes || tail >= page_bytes || (head & (qi_descriptor_bytes - 1)) != 0 ||
        (tail & (qi_descriptor_bytes - 1)) != 0 || head != tail || tail != state.queued_invalidation_tail_offset)
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::queued_invalidation_queue_busy;
        return false;
    }

    auto *queue_bytes = static_cast<u8 *>(state.queued_invalidation_queue_virtual);
    auto *first_descriptor = reinterpret_cast<queued_invalidation_descriptor *>(queue_bytes + tail);
    auto *wait_descriptor = reinterpret_cast<queued_invalidation_descriptor *>(queue_bytes + ((tail + 16) & page_mask));
    const u32 status_token =
        state.queued_invalidation_status_sequence + 1 == 0 ? 1 : state.queued_invalidation_status_sequence + 1;
    state.queued_invalidation_status_sequence = status_token;
    *static_cast<volatile u32 *>(state.queued_invalidation_status_virtual) = 0;
    *first_descriptor = descriptor;
    *wait_descriptor =
        queued_invalidation_descriptor{qi_invalidation_wait_type | qi_invalidation_wait_status_write |
                                           qi_invalidation_wait_fence | (static_cast<u64>(status_token) << 32),
                                       state.queued_invalidation_status_physical};

    if (!state.coherent &&
        (!state.operations.flush_page(state.operations.context, state.queued_invalidation_status_virtual) ||
         !state.operations.flush_page(state.operations.context, state.queued_invalidation_queue_virtual)))
    {
        failure = error::cache_maintenance_unavailable;
        return false;
    }
    write_memory_barrier();

    const u32 new_tail = static_cast<u32>((tail + 2 * qi_descriptor_bytes) & page_mask);
    if (!state.operations.write64(state.operations.context, state.register_base, queued_invalidation_tail_register,
                                  new_tail))
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    write_memory_barrier();

    const auto *status_value = static_cast<const volatile u32 *>(state.queued_invalidation_status_virtual);
    bool completion_observed = false;
    for (u32 attempt = 0; attempt < max_register_polls; ++attempt)
    {
        const u32 observed = *status_value;
        if (observed == status_token)
        {
            completion_observed = true;
            break;
        }
        if (observed != 0)
        {
            state.queued_invalidation_state_uncertain = true;
            failure = error::invalid_capability;
            return false;
        }
    }
    if (!completion_observed)
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::hardware_timeout;
        return false;
    }

    bool head_caught_up = false;
    for (u32 attempt = 0; attempt < max_register_polls; ++attempt)
    {
        if (!state.operations.read64(state.operations.context, state.register_base, queued_invalidation_head_register,
                                     head_register))
        {
            state.queued_invalidation_state_uncertain = true;
            failure = error::register_io_failed;
            return false;
        }
        if ((head_register & ~qi_register_offset_mask) != 0)
        {
            state.queued_invalidation_state_uncertain = true;
            failure = error::invalid_capability;
            return false;
        }
        if ((head_register & qi_register_offset_mask) == new_tail)
        {
            head_caught_up = true;
            break;
        }
    }
    if (!head_caught_up)
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::hardware_timeout;
        return false;
    }

    state.queued_invalidation_tail_offset = new_tail;
    return true;
}

bool install_context_for_unit(unit_state &state, u8 bus, u8 device, u8 function, u64 second_level_root,
                              u8 second_level_address_width, bool page_table_coherent, u16 domain_id,
                              bool &domain_id_published, error &failure)
{
    failure = error::none;
    domain_id_published = false;
    if (!state.translation_enabled || state.state_uncertain || state.root_table_virtual == nullptr ||
        state.operations.allocate_page == nullptr || state.operations.release_page == nullptr ||
        state.operations.write64 == nullptr || state.operations.read64 == nullptr)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if (device >= 32 || function >= 8)
    {
        failure = error::invalid_device_id;
        return false;
    }
    if (second_level_root == 0 || (second_level_root & page_mask) != 0 || page_table_coherent != state.coherent ||
        !address_range_fits_width(second_level_root, page_bytes, static_cast<u32>(state.host_address_width) + 1))
    {
        failure = error::invalid_page_table;
        return false;
    }
    if (domain_id < 2)
    {
        failure = error::invalid_page_table;
        return false;
    }

    const u8 width_encoding = adjusted_width_encoding(second_level_address_width);
    if (width_encoding == 0xff || second_level_address_width > state.maximum_guest_address_width ||
        (state.supported_adjusted_guest_address_widths & (1u << width_encoding)) == 0)
    {
        failure = error::unsupported_page_table_width;
        return false;
    }

    auto *root_entries = static_cast<translation_entry *>(state.root_table_virtual);
    void *context_page = state.context_tables[bus];
    if (context_page == nullptr)
    {
        if ((root_entries[bus].low & context_present) != 0 || root_entries[bus].high != 0)
        {
            state.state_uncertain = true;
            failure = error::invalid_root_page;
            return false;
        }
        u64 context_page_physical = 0;
        if (!state.operations.allocate_page(state.operations.context, context_page, context_page_physical) ||
            context_page == nullptr)
        {
            failure = error::allocation_failed;
            return false;
        }
        if ((reinterpret_cast<u64>(context_page) & page_mask) != 0 || context_page_physical == 0 ||
            (context_page_physical & page_mask) != 0 ||
            !address_range_fits_width(context_page_physical, page_bytes,
                                      static_cast<u32>(state.host_address_width) + 1))
        {
            state.operations.release_page(state.operations.context, context_page);
            failure = error::invalid_root_page;
            return false;
        }
        clear_table_page(context_page);
        if (!flush_table_page(state, context_page))
        {
            state.operations.release_page(state.operations.context, context_page);
            failure = error::cache_maintenance_unavailable;
            return false;
        }

        // Retain the table before publishing its physical address. A failed
        // cache-maintenance callback leaves the hardware-visible result unclear.
        state.context_tables[bus] = context_page;
        state.context_table_physical[bus] = context_page_physical;
        root_entries[bus].low = context_page_physical | context_present;
        root_entries[bus].high = 0;
        write_memory_barrier();
        if (!flush_table_page(state, state.root_table_virtual))
        {
            state.state_uncertain = true;
            failure = error::cache_maintenance_unavailable;
            return false;
        }
    }
    else if ((root_entries[bus].low & context_present) == 0 ||
             (root_entries[bus].low & context_pointer_mask) != state.context_table_physical[bus] ||
             root_entries[bus].high != 0)
    {
        state.state_uncertain = true;
        failure = error::invalid_root_page;
        return false;
    }

    auto *context_entries = static_cast<translation_entry *>(context_page);
    const u32 context_index = static_cast<u32>(device) * 8 + function;
    auto &context_entry = context_entries[context_index];
    if ((context_entry.low & context_present) != 0)
    {
        failure = error::context_already_present;
        return false;
    }

    // Translation type 00b selects multi-level second-level translation.
    context_entry.low = second_level_root | context_present;
    context_entry.high = width_encoding | (static_cast<u64>(domain_id) << 8);
    domain_id_published = true;
    write_memory_barrier();
    if (!flush_table_page(state, context_page))
    {
        state.state_uncertain = true;
        failure = error::cache_maintenance_unavailable;
        return false;
    }
    return invalidate_device_translations(state, failure);
}

bool remove_context_from_unit(unit_state &state, u8 bus, u8 device, u8 function, u16 domain_id, error &failure)
{
    failure = error::none;
    if (!state.translation_enabled || state.state_uncertain || state.root_table_virtual == nullptr)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if (device >= 32 || function >= 8)
    {
        failure = error::invalid_device_id;
        return false;
    }
    if (domain_id < 2)
    {
        failure = error::context_domain_mismatch;
        return false;
    }
    void *context_page = state.context_tables[bus];
    if (context_page == nullptr)
    {
        failure = error::context_not_present;
        return false;
    }
    auto *context_entries = static_cast<translation_entry *>(context_page);
    const u32 context_index = static_cast<u32>(device) * 8 + function;
    auto &context_entry = context_entries[context_index];
    if ((context_entry.low & context_present) == 0)
    {
        failure = error::context_not_present;
        return false;
    }
    if (((context_entry.high & context_domain_id_mask) >> context_domain_id_shift) != domain_id)
    {
        failure = error::context_domain_mismatch;
        return false;
    }
    context_entry.low = 0;
    context_entry.high = 0;
    write_memory_barrier();
    if (!flush_table_page(state, context_page))
    {
        state.state_uncertain = true;
        failure = error::cache_maintenance_unavailable;
        return false;
    }
    return invalidate_device_translations(state, failure);
}

} // namespace

bool enable_default_deny(const dmar_hardware_unit &unit, u8 host_address_width, const register_ops &operations,
                         unit_state &state, error &failure)
{
    failure = error::none;
    state = {};
    if (unit.register_base == 0 || unit.register_length < fault_event_control_register + sizeof(u32) ||
        unit.register_base > ~u64(0) - unit.register_length || (unit.register_base & page_mask) != 0)
    {
        failure = error::invalid_register_aperture;
        return false;
    }

    if (operations.read32 == nullptr || operations.write32 == nullptr || operations.read64 == nullptr ||
        operations.write64 == nullptr || operations.allocate_page == nullptr || operations.release_page == nullptr)
    {
        failure = error::register_io_failed;
        return false;
    }

    u32 version = 0;
    u32 status = 0;
    u32 fault_event_control = 0;
    u64 capability = 0;
    u64 extended_capability = 0;
    if (!operations.read32(operations.context, unit.register_base, version_register, version) ||
        !operations.read64(operations.context, unit.register_base, capability_register, capability) ||
        !operations.read64(operations.context, unit.register_base, extended_capability_register, extended_capability) ||
        !operations.read32(operations.context, unit.register_base, global_status_register, status) ||
        !operations.read32(operations.context, unit.register_base, fault_event_control_register, fault_event_control))
    {
        failure = error::register_io_failed;
        return false;
    }
    if (version == 0 || version == ~u32(0) || capability == 0 || capability == ~u64(0) ||
        extended_capability == ~u64(0))
    {
        failure = error::invalid_capability;
        return false;
    }
    if ((status & status_translation_enable) != 0 || (status & status_queued_invalidation_enable) != 0 ||
        (status & status_interrupt_remapping_enable) != 0)
    {
        failure = error::translation_pre_enabled;
        return false;
    }
    if ((fault_event_control & fault_event_interrupt_mask) == 0)
    {
        failure = error::fault_event_interrupt_pre_enabled;
        return false;
    }

    const u32 maximum_guest_address_width = static_cast<u32>((capability >> 16) & 0x3f) + 1;
    if (host_address_width > 63 || maximum_guest_address_width < 30)
    {
        failure = error::unsupported_address_width;
        return false;
    }
    state.maximum_guest_address_width = static_cast<u8>(maximum_guest_address_width);
    state.supported_adjusted_guest_address_widths = static_cast<u8>((capability >> 8) & 0x1f);
    state.segment = unit.segment;
    state.include_all = unit.include_all;
    state.host_address_width = host_address_width;
    state.register_base = unit.register_base;
    state.register_length = unit.register_length;
    state.extended_capability = extended_capability;
    state.operations = operations;

    const u32 fault_record_offset = static_cast<u32>((capability >> 24) & 0x3ff) * fault_record_bytes;
    const u32 fault_record_count = static_cast<u32>((capability >> 40) & 0xff) + 1;
    const u64 fault_record_bytes_total = static_cast<u64>(fault_record_count) * fault_record_bytes;
    if (fault_record_offset != 0 && fault_record_offset <= unit.register_length &&
        fault_record_bytes_total <= unit.register_length - fault_record_offset)
    {
        state.fault_record_offset = fault_record_offset;
        state.fault_record_count = static_cast<u16>(fault_record_count);
    }

    const u32 iotlb_offset = static_cast<u32>((extended_capability >> 8) & 0x3ff) * 16;
    if (iotlb_offset < context_command_register + sizeof(u64) || iotlb_offset > unit.register_length ||
        2 * sizeof(u64) > unit.register_length - iotlb_offset)
    {
        failure = error::invalid_register_aperture;
        return false;
    }
    state.iotlb_register_offset = iotlb_offset;

    const bool coherent = (extended_capability & 1) != 0;
    state.coherent = coherent;
    if (!coherent && operations.flush_page == nullptr)
    {
        failure = error::cache_maintenance_unavailable;
        return false;
    }

    void *root_table = nullptr;
    u64 root_table_physical = 0;
    if (!operations.allocate_page(operations.context, root_table, root_table_physical) || root_table == nullptr)
    {
        failure = error::allocation_failed;
        return false;
    }

    if ((reinterpret_cast<u64>(root_table) & page_mask) != 0 || (root_table_physical & page_mask) != 0 ||
        !address_fits_width(root_table_physical, static_cast<u32>(host_address_width) + 1) ||
        !address_fits_width(root_table_physical, maximum_guest_address_width))
    {
        operations.release_page(operations.context, root_table);
        failure = error::invalid_root_page;
        return false;
    }

    auto *entries = static_cast<u64 *>(root_table);
    for (u64 index = 0; index < root_entries; ++index)
        entries[index] = 0;
    if ((!coherent && !operations.flush_page(operations.context, root_table)))
    {
        operations.release_page(operations.context, root_table);
        failure = error::cache_maintenance_unavailable;
        return false;
    }
    write_memory_barrier();

    state.root_table_virtual = root_table;
    state.root_table_physical = root_table_physical;
    if (!operations.write64(operations.context, unit.register_base, root_table_address_register, root_table_physical))
    {
        // The write callback may have reached hardware before reporting an
        // error. Keep the root page alive rather than risk a dangling RTADDR.
        state.root_table_retained = true;
        state.state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    state.root_table_retained = true;
    write_memory_barrier();

    if (!operations.write32(operations.context, unit.register_base, global_command_register,
                            command_set_root_table_pointer))
    {
        state.state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto root_pointer_poll = wait_status32(operations, unit.register_base, status_root_table_pointer, true);
    if (root_pointer_poll != poll_result::complete)
    {
        state.state_uncertain = true;
        failure = root_pointer_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }
    state.root_pointer_installed = true;

    if (!operations.write64(operations.context, unit.register_base, context_command_register,
                            context_command_invalidate))
    {
        state.state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto context_poll =
        wait_register64_clear(operations, unit.register_base, context_command_register, context_command_in_progress);
    if (context_poll != poll_result::complete)
    {
        state.state_uncertain = true;
        failure = context_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }

    if (!operations.write64(operations.context, unit.register_base, iotlb_offset, iotlb_command_invalidate))
    {
        state.state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto iotlb_poll =
        wait_register64_clear(operations, unit.register_base, iotlb_offset, iotlb_command_in_progress);
    if (iotlb_poll != poll_result::complete)
    {
        state.state_uncertain = true;
        failure = iotlb_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }

    write_memory_barrier();
    if (!operations.write32(operations.context, unit.register_base, global_command_register,
                            command_translation_enable))
    {
        state.state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto translation_poll = wait_status32(operations, unit.register_base, status_translation_enable, true);
    if (translation_poll != poll_result::complete)
    {
        state.state_uncertain = true;
        failure = translation_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }

    state.translation_enabled = true;
    state.global_command = command_translation_enable;
    return true;
}

bool drain_fault_records(unit_state &state, fault_record *records, u32 capacity, fault_drain_result &result,
                         error &failure)
{
    result = {};
    failure = error::none;
    if (records == nullptr || capacity < state.fault_record_count || state.fault_record_offset == 0 ||
        state.fault_record_count == 0 || state.operations.read32 == nullptr || state.operations.read64 == nullptr ||
        state.operations.write32 == nullptr)
    {
        failure = error::invalid_fault_record;
        return false;
    }

    u32 status = 0;
    if (!state.operations.read32(state.operations.context, state.register_base, fault_status_register, status))
    {
        failure = error::register_io_failed;
        return false;
    }
    result.status = status;
    result.overflow = (status & fault_status_overflow) != 0;

    const bool fault_pending = (status & fault_status_pending) != 0;
    u32 record_index = (status & fault_status_record_index_mask) >> 8;
    if (fault_pending && record_index >= state.fault_record_count)
    {
        failure = error::invalid_fault_record;
        return false;
    }

    for (u32 scanned = 0; fault_pending && scanned < state.fault_record_count && result.count < capacity; ++scanned)
    {
        const u32 record_offset = state.fault_record_offset + record_index * fault_record_bytes;
        u32 high_dword = 0;
        if (!state.operations.read32(state.operations.context, state.register_base, record_offset + 12, high_dword))
        {
            failure = error::register_io_failed;
            return false;
        }
        if ((high_dword & fault_record_valid) == 0)
            break;

        u64 address = 0;
        u64 high = 0;
        if (!state.operations.read64(state.operations.context, state.register_base, record_offset, address) ||
            !state.operations.read64(state.operations.context, state.register_base, record_offset + 8, high))
        {
            failure = error::register_io_failed;
            return false;
        }
        if ((high & (1ull << 63)) == 0)
        {
            result.more = true;
            failure = error::invalid_fault_record;
            return false;
        }

        records[result.count++] = fault_record{
            address & ~page_mask,
            static_cast<u16>(high & 0xffff),
            static_cast<u8>((high >> 32) & 0xff),
            false,
        };

        if (!state.operations.write32(state.operations.context, state.register_base, record_offset + 12,
                                      fault_record_valid))
        {
            failure = error::register_io_failed;
            return false;
        }
        record_index = (record_index + 1) % state.fault_record_count;
    }

    if (fault_pending && result.count == capacity)
    {
        const u32 next_record_offset = state.fault_record_offset + record_index * fault_record_bytes;
        u32 high_dword = 0;
        if (!state.operations.read32(state.operations.context, state.register_base, next_record_offset + 12,
                                     high_dword))
        {
            failure = error::register_io_failed;
            return false;
        }
        result.more = (high_dword & fault_record_valid) != 0;
    }

    if (!result.more && (status & fault_status_clear_mask) != 0 &&
        !state.operations.write32(state.operations.context, state.register_base, fault_status_register,
                                  status & fault_status_clear_mask))
    {
        failure = error::register_io_failed;
        return false;
    }
    return true;
}

bool configure_fault_event_interrupt(unit_state &state, u32 destination_apic_id, u8 vector, error &failure)
{
    failure = error::none;
    fault_event_message message{};
    if (!encode_xapic_fault_event_message(destination_apic_id, vector, message))
    {
        failure = error::invalid_interrupt_route;
        return false;
    }
    if (!state.translation_enabled || state.state_uncertain || state.fault_event_interrupt_enabled ||
        state.fault_event_interrupt_state_uncertain || state.fault_record_offset == 0 || state.fault_record_count == 0)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if (state.register_length < fault_event_upper_address_register + sizeof(u32))
    {
        failure = error::invalid_register_aperture;
        return false;
    }
    if (state.operations.read32 == nullptr || state.operations.write32 == nullptr)
    {
        failure = error::register_io_failed;
        return false;
    }

    u32 control = 0;
    if (!state.operations.read32(state.operations.context, state.register_base, fault_event_control_register, control))
    {
        failure = error::register_io_failed;
        return false;
    }
    if ((control & fault_event_interrupt_mask) == 0)
    {
        failure = error::fault_event_interrupt_pre_enabled;
        return false;
    }

    if (!state.operations.write32(state.operations.context, state.register_base, fault_event_address_register,
                                  message.address) ||
        !state.operations.write32(state.operations.context, state.register_base, fault_event_upper_address_register,
                                  message.upper_address) ||
        !state.operations.write32(state.operations.context, state.register_base, fault_event_data_register,
                                  message.data))
    {
        failure = error::register_io_failed;
        return false;
    }
    write_memory_barrier();

    u32 programmed_address = 0;
    u32 programmed_upper_address = 0;
    u32 programmed_data = 0;
    if (!state.operations.read32(state.operations.context, state.register_base, fault_event_address_register,
                                 programmed_address) ||
        !state.operations.read32(state.operations.context, state.register_base, fault_event_upper_address_register,
                                 programmed_upper_address) ||
        !state.operations.read32(state.operations.context, state.register_base, fault_event_data_register,
                                 programmed_data) ||
        programmed_address != message.address || programmed_upper_address != message.upper_address ||
        programmed_data != message.data)
    {
        failure = error::register_io_failed;
        return false;
    }

    if (!state.operations.write32(state.operations.context, state.register_base, fault_event_control_register, 0))
    {
        state.fault_event_interrupt_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }

    if (!state.operations.read32(state.operations.context, state.register_base, fault_event_control_register, control))
    {
        state.fault_event_interrupt_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    if ((control & fault_event_interrupt_mask) != 0)
    {
        state.fault_event_interrupt_state_uncertain = true;
        failure = error::hardware_timeout;
        return false;
    }

    state.fault_event_interrupt_enabled = true;
    return true;
}

bool initialize_queued_invalidation(unit_state &state, error &failure)
{
    failure = error::none;
    if (!state.translation_enabled || state.state_uncertain || state.queued_invalidation_enabled ||
        state.queued_invalidation_storage_retained || state.operations.read32 == nullptr ||
        state.operations.write32 == nullptr || state.operations.read64 == nullptr ||
        state.operations.write64 == nullptr || state.operations.allocate_page == nullptr ||
        state.operations.release_page == nullptr)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if ((state.extended_capability & extended_capability_queued_invalidation) == 0)
    {
        failure = error::queued_invalidation_unsupported;
        return false;
    }
    if (state.register_length < queued_invalidation_address_register + sizeof(u64))
    {
        failure = error::invalid_register_aperture;
        return false;
    }

    u32 status = 0;
    if (!state.operations.read32(state.operations.context, state.register_base, global_status_register, status))
    {
        failure = error::register_io_failed;
        return false;
    }
    if ((status & status_queued_invalidation_enable) != 0)
    {
        failure = error::queued_invalidation_pre_enabled;
        return false;
    }

    void *queue_page = nullptr;
    u64 queue_physical = 0;
    if (!state.operations.allocate_page(state.operations.context, queue_page, queue_physical) || queue_page == nullptr)
    {
        if (queue_page != nullptr)
            state.operations.release_page(state.operations.context, queue_page);
        failure = error::allocation_failed;
        return false;
    }

    void *status_page = nullptr;
    u64 status_physical = 0;
    if (!state.operations.allocate_page(state.operations.context, status_page, status_physical) ||
        status_page == nullptr)
    {
        if (status_page != nullptr)
            state.operations.release_page(state.operations.context, status_page);
        state.operations.release_page(state.operations.context, queue_page);
        failure = error::allocation_failed;
        return false;
    }

    const u32 host_physical_width = static_cast<u32>(state.host_address_width) + 1;
    const bool pages_valid = (reinterpret_cast<u64>(queue_page) & page_mask) == 0 &&
                             (reinterpret_cast<u64>(status_page) & page_mask) == 0 && queue_physical != 0 &&
                             status_physical != 0 && (queue_physical & page_mask) == 0 &&
                             (status_physical & page_mask) == 0 &&
                             address_range_fits_width(queue_physical, page_bytes, host_physical_width) &&
                             address_range_fits_width(status_physical, page_bytes, host_physical_width);
    if (!pages_valid)
    {
        state.operations.release_page(state.operations.context, status_page);
        state.operations.release_page(state.operations.context, queue_page);
        failure = error::invalid_root_page;
        return false;
    }

    clear_table_page(queue_page);
    clear_table_page(status_page);
    if (!state.coherent && (!state.operations.flush_page(state.operations.context, queue_page) ||
                            !state.operations.flush_page(state.operations.context, status_page)))
    {
        state.operations.release_page(state.operations.context, status_page);
        state.operations.release_page(state.operations.context, queue_page);
        failure = error::cache_maintenance_unavailable;
        return false;
    }

    state.queued_invalidation_queue_virtual = queue_page;
    state.queued_invalidation_queue_physical = queue_physical;
    state.queued_invalidation_status_virtual = status_page;
    state.queued_invalidation_status_physical = status_physical;
    // IQA may reach hardware even when the register callback reports failure.
    // From this point onward both pages remain pinned for the lifetime of the unit.
    state.queued_invalidation_storage_retained = true;
    write_memory_barrier();

    if (!state.operations.write64(state.operations.context, state.register_base, queued_invalidation_address_register,
                                  queue_physical))
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    write_memory_barrier();

    const u32 command = state.global_command | command_queued_invalidation_enable;
    if (!state.operations.write32(state.operations.context, state.register_base, global_command_register, command))
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto enable_poll =
        wait_status32(state.operations, state.register_base, status_queued_invalidation_enable, true);
    if (enable_poll != poll_result::complete)
    {
        state.queued_invalidation_state_uncertain = true;
        failure = enable_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }

    u64 head = 0;
    u64 tail = 0;
    if (!state.operations.read64(state.operations.context, state.register_base, queued_invalidation_head_register,
                                 head) ||
        !state.operations.read64(state.operations.context, state.register_base, queued_invalidation_tail_register,
                                 tail))
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }

    const u64 head_offset = head & qi_register_offset_mask;
    const u64 tail_offset = tail & qi_register_offset_mask;
    if ((head & ~qi_register_offset_mask) != 0 || (tail & ~qi_register_offset_mask) != 0 || head_offset >= page_bytes ||
        tail_offset >= page_bytes || (head_offset & (qi_descriptor_bytes - 1)) != 0 ||
        (tail_offset & (qi_descriptor_bytes - 1)) != 0)
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::invalid_capability;
        return false;
    }
    if (head_offset != tail_offset)
    {
        state.queued_invalidation_state_uncertain = true;
        failure = error::queued_invalidation_queue_busy;
        return false;
    }

    state.queued_invalidation_tail_offset = static_cast<u32>(tail_offset);
    state.global_command = command;
    state.queued_invalidation_enabled = true;
    return true;
}

bool prepare_interrupt_remapping(unit_state &state, error &failure)
{
    failure = error::none;
    if (!state.translation_enabled || state.state_uncertain || !state.queued_invalidation_enabled ||
        state.queued_invalidation_state_uncertain || state.interrupt_remapping_table_retained ||
        state.interrupt_remapping_table_ready || state.operations.read32 == nullptr ||
        state.operations.write32 == nullptr || state.operations.read64 == nullptr ||
        state.operations.write64 == nullptr || state.operations.allocate_page == nullptr ||
        state.operations.release_page == nullptr)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if ((state.extended_capability & extended_capability_interrupt_remapping) == 0)
    {
        failure = error::interrupt_remapping_unsupported;
        return false;
    }
    if (state.register_length < interrupt_remapping_table_address_register + sizeof(u64))
    {
        failure = error::invalid_register_aperture;
        return false;
    }

    u32 status = 0;
    if (!state.operations.read32(state.operations.context, state.register_base, global_status_register, status))
    {
        failure = error::register_io_failed;
        return false;
    }
    if ((status & status_interrupt_remapping_enable) != 0)
    {
        failure = error::interrupt_remapping_pre_enabled;
        return false;
    }

    void *table_page = nullptr;
    u64 table_physical = 0;
    if (!state.operations.allocate_page(state.operations.context, table_page, table_physical) || table_page == nullptr)
    {
        if (table_page != nullptr)
            state.operations.release_page(state.operations.context, table_page);
        failure = error::allocation_failed;
        return false;
    }
    if ((reinterpret_cast<u64>(table_page) & page_mask) != 0 || table_physical == 0 ||
        (table_physical & page_mask) != 0 ||
        !address_range_fits_width(table_physical, page_bytes, static_cast<u32>(state.host_address_width) + 1))
    {
        state.operations.release_page(state.operations.context, table_page);
        failure = error::invalid_root_page;
        return false;
    }

    clear_table_page(table_page);
    if (!state.coherent && !state.operations.flush_page(state.operations.context, table_page))
    {
        state.operations.release_page(state.operations.context, table_page);
        failure = error::cache_maintenance_unavailable;
        return false;
    }

    state.interrupt_remapping_table_virtual = table_page;
    state.interrupt_remapping_table_physical = table_physical;
    // IRTA and SIRTP may become visible even when their completion is uncertain.
    state.interrupt_remapping_table_retained = true;
    write_memory_barrier();

    const u64 table_address = table_physical | interrupt_remapping_table_size_encoding;
    if (!state.operations.write64(state.operations.context, state.register_base,
                                  interrupt_remapping_table_address_register, table_address))
    {
        state.interrupt_remapping_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    write_memory_barrier();

    const u32 command = state.global_command | command_set_interrupt_remapping_table_pointer;
    if (!state.operations.write32(state.operations.context, state.register_base, global_command_register, command))
    {
        state.interrupt_remapping_state_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto pointer_poll =
        wait_status32(state.operations, state.register_base, status_interrupt_remapping_table_pointer, true);
    if (pointer_poll != poll_result::complete)
    {
        state.interrupt_remapping_state_uncertain = true;
        failure = pointer_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }

    const queued_invalidation_descriptor global_iec{qi_interrupt_entry_cache_invalidation_type, 0};
    if (!submit_queued_invalidation(state, global_iec, failure))
    {
        state.interrupt_remapping_state_uncertain = true;
        return false;
    }

    state.interrupt_remapping_table_ready = true;
    return true;
}

bool enable_interrupt_remapping(unit_state &state, error &failure)
{
    failure = error::none;
    if (!state.interrupt_remapping_table_ready || state.interrupt_remapping_state_uncertain ||
        !state.queued_invalidation_enabled || state.queued_invalidation_state_uncertain ||
        (state.extended_capability & extended_capability_interrupt_remapping) == 0 ||
        state.operations.read32 == nullptr || state.operations.write32 == nullptr)
    {
        failure = error::interrupt_remapping_unsupported;
        return false;
    }

    u32 status = 0;
    if (!state.operations.read32(state.operations.context, state.register_base, global_status_register, status))
    {
        state.interrupt_remapping_state_uncertain = true;
        state.interrupt_remapping_enable_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    if ((status & status_interrupt_remapping_enable) != 0)
    {
        if (state.interrupt_remapping_enabled)
            return true;
        state.interrupt_remapping_state_uncertain = true;
        state.interrupt_remapping_enable_uncertain = true;
        failure = error::interrupt_remapping_pre_enabled;
        return false;
    }
    if (state.interrupt_remapping_enabled)
    {
        state.interrupt_remapping_state_uncertain = true;
        state.interrupt_remapping_enable_uncertain = true;
        failure = error::hardware_timeout;
        return false;
    }

    const u32 command = state.global_command | command_interrupt_remapping_enable;
    if (!state.operations.write32(state.operations.context, state.register_base, global_command_register, command))
    {
        state.interrupt_remapping_state_uncertain = true;
        state.interrupt_remapping_enable_uncertain = true;
        failure = error::register_io_failed;
        return false;
    }
    const auto enable_poll =
        wait_status32(state.operations, state.register_base, status_interrupt_remapping_enable, true);
    if (enable_poll != poll_result::complete)
    {
        state.interrupt_remapping_state_uncertain = true;
        state.interrupt_remapping_enable_uncertain = true;
        failure = enable_poll == poll_result::io_failure ? error::register_io_failed : error::hardware_timeout;
        return false;
    }

    state.global_command = command;
    state.interrupt_remapping_enabled = true;
    state.interrupt_remapping_enable_uncertain = false;
    return true;
}

namespace
{
bool install_encoded_irte(unit_state &state, const interrupt_remapping_entry &encoded, u16 &index, error &failure)
{
    failure = error::none;
    index = 0xffff;
    if (!state.interrupt_remapping_table_ready || state.interrupt_remapping_state_uncertain ||
        state.queued_invalidation_state_uncertain ||
        state.next_interrupt_remapping_index >= interrupt_remapping_entry_count ||
        state.interrupt_remapping_table_virtual == nullptr)
    {
        failure = state.next_interrupt_remapping_index >= interrupt_remapping_entry_count
                      ? error::interrupt_remapping_table_full
                      : error::invalid_inventory;
        return false;
    }

    const u16 reserved_index = state.next_interrupt_remapping_index++;
    auto *entries = static_cast<interrupt_remapping_entry *>(state.interrupt_remapping_table_virtual);
    auto &entry = entries[reserved_index];
    entry.high = encoded.high;
    entry.low = encoded.low & ~u64{1};
    write_memory_barrier();
    if (!flush_table_page(state, state.interrupt_remapping_table_virtual))
    {
        state.interrupt_remapping_state_uncertain = true;
        failure = error::cache_maintenance_unavailable;
        return false;
    }

    // Publish P only after the requester ID and destination/vector fields
    // have reached the remapping unit's view of the table.
    entry.low = encoded.low;
    write_memory_barrier();
    if (!flush_table_page(state, state.interrupt_remapping_table_virtual))
    {
        state.interrupt_remapping_state_uncertain = true;
        failure = error::cache_maintenance_unavailable;
        return false;
    }

    const queued_invalidation_descriptor global_iec{qi_interrupt_entry_cache_invalidation_type, 0};
    if (!submit_queued_invalidation(state, global_iec, failure))
    {
        state.interrupt_remapping_state_uncertain = true;
        return false;
    }

    index = reserved_index;
    return true;
}
} // namespace

bool install_pci_msi_irte(unit_state &state, u16 requester_id, u32 destination_apic_id, u8 vector,
                          bool extended_interrupt_mode, u16 &index, error &failure, bool &outcome_uncertain)
{
    failure = error::none;
    index = 0xffff;
    outcome_uncertain = false;
    if (extended_interrupt_mode)
    {
        // IRTA.EIME is deliberately left clear until the kernel has a verified
        // x2APIC-mode handoff. Encoding an extended destination into an xAPIC
        // IR table would otherwise produce a syntactically valid, wrong route.
        failure = error::interrupt_remapping_unsupported;
        return false;
    }

    interrupt_remapping_entry encoded{};
    if (!encode_pci_msi_irte(requester_id, destination_apic_id, vector, extended_interrupt_mode, encoded))
    {
        failure = error::invalid_interrupt_route;
        return false;
    }
    const bool ir_state_was_uncertain = state.interrupt_remapping_state_uncertain;
    const bool qi_state_was_uncertain = state.queued_invalidation_state_uncertain;
    const bool installed = install_encoded_irte(state, encoded, index, failure);
    outcome_uncertain = !installed && ((!ir_state_was_uncertain && state.interrupt_remapping_state_uncertain) ||
                                       (!qi_state_was_uncertain && state.queued_invalidation_state_uncertain));
    return installed;
}

bool install_ioapic_irte(unit_state &state, u16 source_id, u32 destination_apic_id, u8 vector, bool level_triggered,
                         u16 &index, error &failure)
{
    failure = error::none;
    index = 0xffff;
    interrupt_remapping_entry encoded{};
    if (!encode_ioapic_irte(source_id, destination_apic_id, vector, level_triggered, false, encoded))
    {
        failure = error::invalid_interrupt_route;
        return false;
    }
    return install_encoded_irte(state, encoded, index, failure);
}

bool disable_interrupt_remapping_entry(unit_state &state, u16 index, error &failure)
{
    failure = error::none;
    if (!state.interrupt_remapping_table_ready || state.interrupt_remapping_state_uncertain ||
        state.queued_invalidation_state_uncertain || state.interrupt_remapping_table_virtual == nullptr ||
        index >= state.next_interrupt_remapping_index || index >= interrupt_remapping_entry_count)
    {
        failure = error::invalid_inventory;
        return false;
    }

    auto *entries = static_cast<interrupt_remapping_entry *>(state.interrupt_remapping_table_virtual);
    auto &entry = entries[index];
    if ((entry.low & 1) == 0)
    {
        failure = error::interrupt_remapping_entry_not_present;
        return false;
    }

    entry.low &= ~u64{1};
    write_memory_barrier();
    if (!flush_table_page(state, state.interrupt_remapping_table_virtual))
    {
        state.interrupt_remapping_state_uncertain = true;
        failure = error::cache_maintenance_unavailable;
        return false;
    }

    const queued_invalidation_descriptor global_iec{qi_interrupt_entry_cache_invalidation_type, 0};
    if (!submit_queued_invalidation(state, global_iec, failure))
    {
        state.interrupt_remapping_state_uncertain = true;
        return false;
    }
    return true;
}

bool controller::initialize(const firmware_inventory &snapshot, const register_ops &operations, error &failure)
{
    failure = error::none;
    if (attempted_)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if (snapshot.dmar_state != dmar_inventory_state::valid || snapshot.dmar_units.empty())
    {
        failure = error::invalid_inventory;
        return false;
    }
    if (!snapshot.dmar_reserved_regions.empty())
    {
        failure = error::reserved_memory_requires_domain;
        return false;
    }
    if (snapshot.dmar_units.size() > max_hardware_units)
    {
        failure = error::too_many_units;
        return false;
    }

    attempted_ = true;
    unit_count_ = static_cast<u32>(snapshot.dmar_units.size());
    queued_invalidation_unit_count_ = 0;
    interrupt_remapping_table_ready_unit_count_ = 0;
    for (u32 index = 0; index < unit_count_; ++index)
    {
        if (!enable_default_deny(snapshot.dmar_units.data()[index], snapshot.dmar_host_address_width, operations,
                                 units_[index], failure))
        {
            for (u32 prior = 0; prior < index; ++prior)
                active_unit_count_ += units_[prior].translation_enabled ? 1u : 0u;
            return false;
        }
        active_unit_count_++;
    }

    for (u32 index = 0; index < unit_count_; ++index)
    {
        auto &unit = units_[index];
        if ((unit.extended_capability & extended_capability_queued_invalidation) == 0)
            continue;

        if (!initialize_queued_invalidation(unit, unit.queued_invalidation_failure))
            continue;
        queued_invalidation_unit_count_++;

        if ((unit.extended_capability & extended_capability_interrupt_remapping) == 0)
            continue;
        if (prepare_interrupt_remapping(unit, unit.interrupt_remapping_failure))
            interrupt_remapping_table_ready_unit_count_++;
    }

    default_deny_ready_ = true;
    return true;
}

bool controller::drain_fault_records_for_unit(u32 unit_index, fault_record *records, u32 capacity,
                                              fault_drain_result &result, error &failure)
{
    failure = error::none;
    result = {};
    if (!default_deny_ready_ || unit_index >= unit_count_)
    {
        failure = error::invalid_inventory;
        return false;
    }
    return drain_fault_records(units_[unit_index], records, capacity, result, failure);
}

bool controller::configure_fault_event_interrupt_for_unit(u32 unit_index, u32 destination_apic_id, u8 vector,
                                                          error &failure)
{
    failure = error::none;
    if (!default_deny_ready_ || unit_index >= unit_count_)
    {
        failure = error::invalid_inventory;
        return false;
    }
    return configure_fault_event_interrupt(units_[unit_index], destination_apic_id, vector, failure);
}

bool controller::fault_event_interrupt_enabled_for_unit(u32 unit_index) const
{
    return default_deny_ready_ && unit_index < unit_count_ && units_[unit_index].fault_event_interrupt_enabled &&
           !units_[unit_index].fault_event_interrupt_state_uncertain;
}

bool controller::fault_event_interrupt_state_uncertain_for_unit(u32 unit_index) const
{
    return unit_index < unit_count_ && units_[unit_index].fault_event_interrupt_state_uncertain;
}

bool controller::interrupt_remapping_ready() const
{
    if (!default_deny_ready_ || unit_count_ == 0)
        return false;
    for (u32 index = 0; index < unit_count_; ++index)
    {
        const auto &unit = units_[index];
        if (!unit.queued_invalidation_enabled || unit.queued_invalidation_state_uncertain ||
            !unit.interrupt_remapping_table_ready || unit.interrupt_remapping_state_uncertain ||
            (unit.extended_capability & extended_capability_interrupt_remapping) == 0)
            return false;
    }
    return true;
}

bool controller::enable_interrupt_remapping(error &failure)
{
    failure = error::none;
    if (!interrupt_remapping_ready())
    {
        failure = error::interrupt_remapping_unsupported;
        return false;
    }
    for (u32 index = 0; index < unit_count_; ++index)
    {
        if (!iommu::intel_vtd::enable_interrupt_remapping(units_[index], failure))
            return false;
    }
    return interrupt_remapping_enabled();
}

bool controller::interrupt_remapping_enabled()
{
    if (!default_deny_ready_ || unit_count_ == 0)
        return false;
    for (u32 index = 0; index < unit_count_; ++index)
    {
        auto &unit = units_[index];
        if (!unit.interrupt_remapping_enabled || unit.interrupt_remapping_state_uncertain ||
            unit.operations.read32 == nullptr)
            return false;
        u32 status = 0;
        if (!unit.operations.read32(unit.operations.context, unit.register_base, global_status_register, status) ||
            (status & status_interrupt_remapping_enable) == 0)
        {
            unit.interrupt_remapping_state_uncertain = true;
            unit.interrupt_remapping_enable_uncertain = true;
            return false;
        }
    }
    return true;
}

bool controller::interrupt_remapping_activation_uncertain() const
{
    for (u32 index = 0; index < unit_count_; ++index)
    {
        if (units_[index].interrupt_remapping_enable_uncertain)
            return true;
    }
    return false;
}

bool controller::install_ioapic_irte_for_unit(u32 unit_index, u16 segment, u16 source_id, u32 destination_apic_id,
                                              u8 vector, bool level_triggered, u16 &entry_index, error &failure)
{
    entry_index = 0xffff;
    if (!default_deny_ready_ || unit_index >= unit_count_ || units_[unit_index].segment != segment)
    {
        failure = error::invalid_inventory;
        return false;
    }
    auto &unit = units_[unit_index];
    if (!unit.interrupt_remapping_table_ready || unit.interrupt_remapping_state_uncertain ||
        !unit.queued_invalidation_enabled || unit.queued_invalidation_state_uncertain)
    {
        failure = error::interrupt_remapping_unsupported;
        return false;
    }
    return iommu::intel_vtd::install_ioapic_irte(unit, source_id, destination_apic_id, vector, level_triggered,
                                                 entry_index, failure);
}

bool controller::install_pci_msi_irte_for_unit(u32 unit_index, u16 segment, u16 requester_id, u32 destination_apic_id,
                                               u8 vector, u16 &entry_index, error &failure, bool &outcome_uncertain)
{
    failure = error::none;
    entry_index = 0xffff;
    outcome_uncertain = false;
    if (!default_deny_ready_ || unit_index >= unit_count_ || units_[unit_index].segment != segment)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if (!interrupt_remapping_enabled())
    {
        failure = error::interrupt_remapping_unsupported;
        return false;
    }

    auto &unit = units_[unit_index];
    if (!unit.interrupt_remapping_table_ready || unit.interrupt_remapping_state_uncertain ||
        !unit.queued_invalidation_enabled || unit.queued_invalidation_state_uncertain)
    {
        failure = error::interrupt_remapping_unsupported;
        return false;
    }
    return iommu::intel_vtd::install_pci_msi_irte(unit, requester_id, destination_apic_id, vector, false, entry_index,
                                                  failure, outcome_uncertain);
}

bool controller::disable_interrupt_remapping_entry_for_unit(u32 unit_index, u16 entry_index, error &failure)
{
    if (!default_deny_ready_ || unit_index >= unit_count_)
    {
        failure = error::invalid_inventory;
        return false;
    }
    return iommu::intel_vtd::disable_interrupt_remapping_entry(units_[unit_index], entry_index, failure);
}

bool controller::install_device_context(u16 segment, u8 bus, u8 device, u8 function, u64 second_level_root,
                                        u8 second_level_address_width, bool page_table_coherent, u16 &domain_id,
                                        error &failure)
{
    failure = error::none;
    domain_id = 0;
    if (!default_deny_ready_)
    {
        failure = error::invalid_inventory;
        return false;
    }

    unit_state *selected = nullptr;
    bool scoped_unit_present = false;
    for (u32 index = 0; index < unit_count_; ++index)
    {
        auto &candidate = units_[index];
        if (candidate.segment != segment)
            continue;
        if (!candidate.include_all)
        {
            scoped_unit_present = true;
            continue;
        }
        if (selected != nullptr)
        {
            failure = error::device_scope_unresolved;
            return false;
        }
        selected = &candidate;
    }
    if (selected == nullptr || scoped_unit_present)
    {
        failure = error::device_scope_unresolved;
        return false;
    }
    return install_device_context(static_cast<u32>(selected - units_), segment, bus, device, function,
                                  second_level_root, second_level_address_width, page_table_coherent, domain_id,
                                  failure);
}

bool controller::install_device_context(u32 unit_index, u16 segment, u8 bus, u8 device, u8 function,
                                        u64 second_level_root, u8 second_level_address_width, bool page_table_coherent,
                                        u16 &domain_id, error &failure)
{
    failure = error::none;
    domain_id = 0;
    if (!default_deny_ready_ || unit_index >= unit_count_ || units_[unit_index].segment != segment)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if (next_domain_id_ > 0xffff)
    {
        failure = error::domain_id_exhausted;
        return false;
    }

    const auto candidate_domain_id = static_cast<u16>(next_domain_id_);
    bool domain_id_published = false;
    const bool installed = install_context_for_unit(units_[unit_index], bus, device, function, second_level_root,
                                                    second_level_address_width, page_table_coherent,
                                                    candidate_domain_id, domain_id_published, failure);
    if (installed || domain_id_published)
        domain_id = candidate_domain_id;
    if (domain_id_published)
        next_domain_id_++;
    return installed;
}

bool controller::remove_device_context(u16 segment, u8 bus, u8 device, u8 function, u16 domain_id, error &failure)
{
    failure = error::none;
    if (!default_deny_ready_)
    {
        failure = error::invalid_inventory;
        return false;
    }

    unit_state *selected = nullptr;
    bool scoped_unit_present = false;
    for (u32 index = 0; index < unit_count_; ++index)
    {
        auto &candidate = units_[index];
        if (candidate.segment != segment)
            continue;
        if (!candidate.include_all)
        {
            scoped_unit_present = true;
            continue;
        }
        if (selected != nullptr)
        {
            failure = error::device_scope_unresolved;
            return false;
        }
        selected = &candidate;
    }
    if (selected == nullptr || scoped_unit_present)
    {
        failure = error::device_scope_unresolved;
        return false;
    }
    return remove_device_context(static_cast<u32>(selected - units_), segment, bus, device, function, domain_id,
                                 failure);
}

bool controller::create_device_domain(u16 segment, u8 bus, u8 device, u8 function, device_domain &domain,
                                      error &failure)
{
    failure = error::none;
    if (domain.valid || domain.page_table.root_virtual != nullptr || !default_deny_ready_)
    {
        failure = error::invalid_inventory;
        return false;
    }

    unit_state *selected = nullptr;
    bool scoped_unit_present = false;
    for (u32 index = 0; index < unit_count_; ++index)
    {
        auto &candidate = units_[index];
        if (candidate.segment != segment)
            continue;
        if (!candidate.include_all)
        {
            scoped_unit_present = true;
            continue;
        }
        if (selected != nullptr)
        {
            failure = error::device_scope_unresolved;
            return false;
        }
        selected = &candidate;
    }
    if (selected == nullptr || scoped_unit_present)
    {
        failure = error::device_scope_unresolved;
        return false;
    }
    return create_device_domain(static_cast<u32>(selected - units_), segment, bus, device, function, domain, failure);
}

bool controller::remove_device_context(u32 unit_index, u16 segment, u8 bus, u8 device, u8 function, u16 domain_id,
                                       error &failure)
{
    failure = error::none;
    if (!default_deny_ready_ || unit_index >= unit_count_ || units_[unit_index].segment != segment)
    {
        failure = error::invalid_inventory;
        return false;
    }
    return remove_context_from_unit(units_[unit_index], bus, device, function, domain_id, failure);
}

bool controller::create_device_domain(u32 unit_index, u16 segment, u8 bus, u8 device, u8 function,
                                      device_domain &domain, error &failure)
{
    failure = error::none;
    if (domain.valid || domain.page_table.root_virtual != nullptr || !default_deny_ready_ ||
        unit_index >= unit_count_ || units_[unit_index].segment != segment)
    {
        failure = error::invalid_inventory;
        return false;
    }
    auto *selected = &units_[unit_index];
    if (selected->operations.physical_to_virtual == nullptr)
    {
        failure = error::invalid_page_table;
        return false;
    }

    u8 address_width = 0;
    constexpr u8 widths[] = {57, 48, 39, 30};
    for (const u8 candidate_width : widths)
    {
        const u8 encoding = adjusted_width_encoding(candidate_width);
        if (candidate_width <= selected->maximum_guest_address_width &&
            (selected->supported_adjusted_guest_address_widths & (1u << encoding)) != 0)
        {
            address_width = candidate_width;
            break;
        }
    }
    if (address_width == 0)
    {
        failure = error::unsupported_address_width;
        return false;
    }

    const page_table_ops page_operations{selected->operations.allocate_page, selected->operations.release_page,
                                         selected->operations.physical_to_virtual, selected->operations.flush_page,
                                         selected->operations.context};
    device_domain candidate{};
    const auto create_failure =
        create_second_level_table(address_width, static_cast<u8>(selected->host_address_width + 1), selected->coherent,
                                  page_operations, candidate.page_table);
    if (create_failure != page_table_error::none)
    {
        failure = translate_page_table_error(create_failure);
        return false;
    }

    candidate.segment = segment;
    candidate.bus = bus;
    candidate.device = device;
    candidate.function = function;
    candidate.unit_index = static_cast<u8>(selected - units_);

    u16 domain_id = 0;
    if (!install_device_context(unit_index, segment, bus, device, function, candidate.page_table.root_physical,
                                address_width, selected->coherent, domain_id, failure))
    {
        if (domain_id != 0)
        {
            // The context entry may be visible to hardware even though cache
            // maintenance or invalidation did not complete. Keep its root
            // table and context identity alive for fail-closed quarantine.
            candidate.domain_id = domain_id;
            candidate.valid = true;
            candidate.context_installed = true;
            candidate.state_uncertain = true;
            domain = candidate;
            return false;
        }
        const auto cleanup = destroy_second_level_table(candidate.page_table);
        if (cleanup != page_table_error::none)
        {
            candidate.state_uncertain = true;
            domain = candidate;
        }
        return false;
    }

    candidate.domain_id = domain_id;
    candidate.valid = true;
    candidate.context_installed = true;
    domain = candidate;
    return true;
}

error controller::map_device_page(device_domain &domain, u64 iova, u64 physical_address, dma_access access)
{
    if (!domain.valid || !domain.context_installed || domain.state_uncertain || domain.unit_index >= unit_count_)
        return domain.state_uncertain ? error::domain_state_uncertain : error::invalid_inventory;
    if (domain.mapped_page_count == ~u64(0))
        return error::domain_mapping_count_overflow;
    auto &unit = units_[domain.unit_index];
    if (!unit.translation_enabled || unit.state_uncertain || unit.segment != domain.segment)
    {
        domain.state_uncertain = true;
        return error::domain_state_uncertain;
    }

    const auto mapping = map_dma_page(domain.page_table, iova, physical_address, access);
    if (mapping != page_table_error::none)
        return translate_page_table_error(mapping);
    error failure = error::none;
    if (!invalidate_device_translations(unit, failure))
    {
        domain.state_uncertain = true;
        return failure;
    }
    domain.mapped_page_count++;
    return error::none;
}

error controller::unmap_device_page(device_domain &domain, u64 iova)
{
    if (!domain.valid || !domain.context_installed || domain.state_uncertain || domain.unit_index >= unit_count_)
        return domain.state_uncertain ? error::domain_state_uncertain : error::invalid_inventory;
    if (domain.mapped_page_count == 0)
        return error::dma_page_not_mapped;
    auto &unit = units_[domain.unit_index];
    if (!unit.translation_enabled || unit.state_uncertain || unit.segment != domain.segment)
    {
        domain.state_uncertain = true;
        return error::domain_state_uncertain;
    }

    const auto mapping = unmap_dma_page(domain.page_table, iova);
    if (mapping != page_table_error::none)
        return translate_page_table_error(mapping);
    error failure = error::none;
    if (!invalidate_device_translations(unit, failure))
    {
        domain.state_uncertain = true;
        return failure;
    }
    domain.mapped_page_count--;
    return error::none;
}

bool controller::destroy_device_domain(device_domain &domain, error &failure)
{
    failure = error::none;
    if (!domain.valid || !domain.context_installed || domain.unit_index >= unit_count_)
    {
        failure = error::invalid_inventory;
        return false;
    }
    if (domain.state_uncertain)
    {
        failure = error::domain_state_uncertain;
        return false;
    }
    if (domain.mapped_page_count != 0)
    {
        failure = error::domain_has_mappings;
        return false;
    }

    if (!remove_device_context(domain.unit_index, domain.segment, domain.bus, domain.device, domain.function,
                               domain.domain_id, failure))
    {
        domain.state_uncertain = true;
        return false;
    }
    domain.context_installed = false;

    const auto destroy_failure = destroy_second_level_table(domain.page_table);
    if (destroy_failure != page_table_error::none)
    {
        domain.state_uncertain = true;
        failure = translate_page_table_error(destroy_failure);
        return false;
    }
    domain = {};
    return true;
}

na_status_t to_status(error failure) noexcept
{
    switch (failure)
    {
        case error::none:
            return NA_STATUS_OK;
        case error::allocation_failed:
        case error::domain_id_exhausted:
        case error::too_many_units:
        case error::domain_mapping_count_overflow:
        case error::interrupt_remapping_table_full:
            return NA_STATUS_RESOURCE_EXHAUSTED;
        case error::unsupported_address_width:
        case error::cache_maintenance_unavailable:
        case error::reserved_memory_requires_domain:
        case error::device_scope_unresolved:
        case error::unsupported_page_table_width:
        case error::queued_invalidation_unsupported:
        case error::queued_invalidation_pre_enabled:
        case error::queued_invalidation_queue_busy:
        case error::interrupt_remapping_unsupported:
        case error::interrupt_remapping_pre_enabled:
        case error::fault_event_interrupt_pre_enabled:
            return NA_STATUS_NOT_SUPPORTED;
        case error::invalid_device_id:
        case error::invalid_register_aperture:
        case error::invalid_capability:
        case error::invalid_root_page:
        case error::invalid_inventory:
        case error::invalid_page_table:
        case error::context_domain_mismatch:
        case error::dma_page_already_mapped:
        case error::dma_page_not_mapped:
        case error::context_already_present:
        case error::context_not_present:
        case error::domain_has_mappings:
        case error::interrupt_remapping_entry_not_present:
        case error::invalid_interrupt_route:
            return NA_STATUS_INVALID_ARGUMENT;
        case error::register_io_failed:
        case error::translation_pre_enabled:
        case error::hardware_timeout:
        case error::domain_state_uncertain:
        case error::invalid_fault_record:
            return NA_STATUS_IO_ERROR;
    }
    return NA_STATUS_IO_ERROR;
}

interrupt_route_token encode_route_token(u32 unit_index, u16 entry_index, u16 segment, u16 requester_id,
                                         u64 generation) noexcept
{
    interrupt_route_token token{};
    token.provider = provider_kind::intel_vtd;
    token.generation = generation;
    token.opaque[0] = (static_cast<u64>(unit_index) << 32) | entry_index;
    token.opaque[1] = (static_cast<u64>(segment) << 16) | requester_id;
    return token;
}

bool decode_route_token(const interrupt_route_token &token, u64 expected_generation, u32 &unit_index, u16 &entry_index,
                        u16 &segment, u16 &requester_id) noexcept
{
    if (!token.valid() || token.provider != provider_kind::intel_vtd || token.generation != expected_generation ||
        ((token.opaque[0] >> 16) & 0xffffu) != 0 || (token.opaque[1] >> 32) != 0)
        return false;
    unit_index = static_cast<u32>(token.opaque[0] >> 32);
    entry_index = static_cast<u16>(token.opaque[0]);
    segment = static_cast<u16>(token.opaque[1] >> 16);
    requester_id = static_cast<u16>(token.opaque[1]);
    return true;
}

bool install_pci_msi_route(controller &backend, const device_source_ref &source, u64 firmware_generation,
                           u32 destination_apic_id, u8 vector, interrupt_route_token &token, msi_message &message,
                           bool &outcome_uncertain, error &failure)
{
    token = {};
    message = {};
    outcome_uncertain = false;
    failure = error::none;
    if (!source.valid() || source.unit.provider != provider_kind::intel_vtd || source.unit.opaque[0] > ~u32{0} ||
        destination_apic_id > 0xff || source.unit.generation != firmware_generation)
    {
        failure = error::invalid_interrupt_route;
        return false;
    }

    const u32 unit_index = static_cast<u32>(source.unit.opaque[0]);
    u16 entry_index = 0xffff;
    if (!backend.install_pci_msi_irte_for_unit(unit_index, source.segment, source.device_id, destination_apic_id,
                                               vector, entry_index, failure, outcome_uncertain))
        return false;

    if (!encode_remappable_msi_message(entry_index, message))
    {
        failure = error::invalid_interrupt_route;
        error cleanup_failure = error::none;
        if (!backend.disable_interrupt_remapping_entry_for_unit(unit_index, entry_index, cleanup_failure))
        {
            failure = cleanup_failure;
            outcome_uncertain = true;
        }
        return false;
    }
    token = encode_route_token(unit_index, entry_index, source.segment, source.device_id, source.unit.generation);
    return true;
}

bool install_ioapic_route(controller &backend, const device_source_ref &source, u64 firmware_generation,
                          u32 destination_apic_id, u8 vector, bool low_polarity, bool level_triggered,
                          ioapic_route_encoding &route, error &failure)
{
    route = {};
    failure = error::none;
    if (!source.valid() || source.unit.provider != provider_kind::intel_vtd || source.unit.opaque[0] > ~u32{0} ||
        destination_apic_id > 0xff || source.unit.generation != firmware_generation)
    {
        failure = error::invalid_interrupt_route;
        return false;
    }

    const u32 unit_index = static_cast<u32>(source.unit.opaque[0]);
    u16 entry_index = 0xffff;
    if (!backend.install_ioapic_irte_for_unit(unit_index, source.segment, source.device_id, destination_apic_id, vector,
                                              level_triggered, entry_index, failure))
        return false;

    if (!encode_ioapic_remappable_rte(entry_index, vector, low_polarity, level_triggered, true,
                                      route.redirection_entry))
    {
        failure = error::invalid_interrupt_route;
        error cleanup_failure = error::none;
        if (!backend.disable_interrupt_remapping_entry_for_unit(unit_index, entry_index, cleanup_failure))
            failure = cleanup_failure;
        route = {};
        return false;
    }
    route.token = encode_route_token(unit_index, entry_index, source.segment, source.device_id, source.unit.generation);
    return true;
}

bool route_matches_pci(const interrupt_route_token &token, const device_source_ref &source) noexcept
{
    if (!source.valid() || source.unit.provider != provider_kind::intel_vtd)
        return false;
    u32 unit_index = ~u32{0};
    u16 entry_index = 0xffff;
    u16 segment = 0xffff;
    u16 requester_id = 0xffff;
    return decode_route_token(token, source.unit.generation, unit_index, entry_index, segment, requester_id) &&
           unit_index == source.unit.opaque[0] && segment == source.segment && requester_id == source.device_id &&
           entry_index < interrupt_remapping_entry_count;
}

bool retire_route(controller &backend, const interrupt_route_token &token, u64 firmware_generation, error &failure)
{
    failure = error::none;
    u32 unit_index = ~u32{0};
    u16 entry_index = 0xffff;
    u16 segment = 0xffff;
    u16 requester_id = 0xffff;
    if (!decode_route_token(token, firmware_generation, unit_index, entry_index, segment, requester_id))
    {
        failure = error::invalid_interrupt_route;
        return false;
    }
    return backend.disable_interrupt_remapping_entry_for_unit(unit_index, entry_index, failure);
}

na_status_t map_dma_page(controller &backend, device_domain &domain, u64 iova, u64 physical_address, dma_access access,
                         bool &outcome_uncertain)
{
    const error failure = backend.map_device_page(domain, iova, physical_address, access);
    outcome_uncertain = domain.state_uncertain;
    return to_status(failure);
}

na_status_t unmap_dma_page(controller &backend, device_domain &domain, u64 iova, bool &outcome_uncertain)
{
    const error failure = backend.unmap_device_page(domain, iova);
    outcome_uncertain = domain.state_uncertain;
    return to_status(failure);
}

na_status_t destroy_dma_domain(controller &backend, device_domain &domain, bool &outcome_uncertain)
{
    error failure = error::none;
    const bool destroyed = backend.destroy_device_domain(domain, failure);
    outcome_uncertain = domain.state_uncertain;
    return destroyed ? NA_STATUS_OK : to_status(failure);
}

resolution_result resolve_pci_source(const firmware_inventory &snapshot, const pci_function_location &function,
                                     u64 generation, bridge_info_reader read_bridge_info, void *context,
                                     device_source_ref &source)
{
    source = {};
    if (generation != snapshot.firmware_generation || function.device >= 32 || function.function >= 8)
        return resolution_result::reject;
    if (snapshot.dmar_state == dmar_inventory_state::absent)
        return resolution_result::not_applicable;
    if (snapshot.dmar_state != dmar_inventory_state::valid)
        return resolution_result::reject;

    u32 unit_index = ~u32{0};
    u16 requester_id = 0;
    source_resolution_error failure = source_resolution_error::none;
    if (!resolve_pci_device_unit(snapshot, function.segment, function.bus, function.device, function.function,
                                 read_bridge_info, context, unit_index, requester_id, failure))
        return failure == source_resolution_error::scope_not_found ? resolution_result::not_applicable
                                                                   : resolution_result::reject;

    source.unit.provider = provider_kind::intel_vtd;
    source.unit.generation = generation;
    source.unit.opaque[0] = unit_index;
    source.segment = function.segment;
    source.device_id = requester_id;
    return resolution_result::resolved;
}

resolution_result resolve_ioapic_source(const firmware_inventory &snapshot, u8 enumeration_id, u32 madt_match_count,
                                        u64 generation, bridge_info_reader read_bridge_info, void *context,
                                        device_source_ref &source)
{
    source = {};
    if (generation != snapshot.firmware_generation)
        return resolution_result::reject;
    if (snapshot.dmar_state == dmar_inventory_state::absent)
        return resolution_result::not_applicable;
    if (snapshot.dmar_state != dmar_inventory_state::valid)
        return resolution_result::reject;
    if (madt_match_count == 0)
        return resolution_result::not_applicable;
    if (madt_match_count != 1)
        return resolution_result::reject;

    const dmar_hardware_unit *matched_unit = nullptr;
    u32 matched_unit_index = 0;
    for (u32 unit_index = 0; unit_index < snapshot.dmar_units.size(); ++unit_index)
    {
        const auto &unit = snapshot.dmar_units.at(unit_index);
        if (unit.device_scope_start > snapshot.dmar_device_scopes.size() ||
            unit.device_scope_count > snapshot.dmar_device_scopes.size() - unit.device_scope_start)
            return resolution_result::reject;
        for (u32 index = 0; index < unit.device_scope_count; ++index)
        {
            const auto &scope = snapshot.dmar_device_scopes.at(unit.device_scope_start + index);
            if (scope.type != dmar_device_scope_type_ioapic || scope.enumeration_id != enumeration_id)
                continue;
            if (matched_unit != nullptr)
                return resolution_result::reject;
            matched_unit = &unit;
            matched_unit_index = unit_index;
        }
    }
    if (matched_unit == nullptr)
        return resolution_result::not_applicable;

    u16 requester_id = 0;
    source_resolution_error failure = source_resolution_error::none;
    if (!resolve_ioapic_requester_id(snapshot, *matched_unit, enumeration_id, read_bridge_info, context, requester_id,
                                     failure))
        return resolution_result::reject;

    source.unit.provider = provider_kind::intel_vtd;
    source.unit.generation = generation;
    source.unit.opaque[0] = matched_unit_index;
    source.segment = matched_unit->segment;
    source.device_id = requester_id;
    return resolution_result::resolved;
}

} // namespace iommu::intel_vtd
