#include <catch2/catch_test_macros.hpp>

#include "iommu_backend_conformance.hpp"
#include "iommu_firmware_test_support.hpp"
#include "kernel/iommu/intel_vtd.hpp"
#include "kernel/iommu/intel_vtd_interrupt.hpp"
#include "kernel/mm/new.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace memory
{
MemoryAllocator *MemoryAllocatorV = nullptr;

void *MemoryAllocator::allocate(u64 size, u64 alignment) noexcept
{
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    void *allocation = nullptr;
    if (::posix_memalign(&allocation, static_cast<size_t>(alignment), static_cast<size_t>(size)) != 0)
        return nullptr;
    return allocation;
}

void MemoryAllocator::deallocate(void *allocation) noexcept { ::free(allocation); }
} // namespace memory

namespace
{
constexpr u32 root_table_pointer_offset = 0x20;
constexpr u32 global_command_offset = 0x18;
constexpr u32 global_status_offset = 0x1c;
constexpr u32 fault_status_offset = 0x34;
constexpr u32 fault_event_control_offset = 0x38;
constexpr u32 fault_event_data_offset = 0x3c;
constexpr u32 fault_event_address_offset = 0x40;
constexpr u32 fault_event_upper_address_offset = 0x44;
constexpr u32 context_command_offset = 0x28;
constexpr u32 iotlb_register_offset = 0x100;
constexpr u32 queued_invalidation_head_offset = 0x80;
constexpr u32 queued_invalidation_tail_offset = 0x88;
constexpr u32 queued_invalidation_address_offset = 0x90;
constexpr u32 interrupt_remapping_table_address_offset = 0xb8;
constexpr u32 root_pointer_status = 1u << 30;
constexpr u32 translation_enable_status = 1u << 31;
constexpr u32 interrupt_remapping_table_pointer_status = 1u << 24;
constexpr u32 queued_invalidation_enable_status = 1u << 26;
constexpr u32 interrupt_remapping_enable_status = 1u << 25;
constexpr u32 set_root_table_pointer = 1u << 30;
constexpr u32 translation_enable = 1u << 31;
constexpr u32 set_interrupt_remapping_table_pointer = 1u << 24;
constexpr u32 queued_invalidation_enable = 1u << 26;
constexpr u32 interrupt_remapping_enable = 1u << 25;
constexpr u64 invalidate_context = (1ull << 63) | (1ull << 61);
constexpr u64 invalidate_iotlb = (1ull << 63) | (1ull << 60);

class test_allocator final : public freelibcxx::Allocator
{
  public:
    void *allocate(size_t size, size_t) noexcept override { return std::malloc(size); }
    void deallocate(void *pointer) noexcept override { std::free(pointer); }
};

struct fake_vtd
{
    alignas(4096) std::array<u8, 4096> root_page{};
    alignas(4096) std::array<std::array<u8, 4096>, 8> additional_pages{};
    u64 capability = (47ull << 16) | (1ull << 10);
    u64 extended_capability = 1ull | (0x10ull << 8);
    u64 root_table_address = 0;
    u64 context_command = 0;
    u64 iotlb_command = 0;
    u64 context_command_written = 0;
    u64 iotlb_command_written = 0;
    u64 queued_invalidation_address = 0;
    u64 queued_invalidation_head = 0;
    u64 queued_invalidation_tail = 0;
    u64 interrupt_remapping_table_address = 0;
    u64 fault_record_low = 0;
    u64 fault_record_high = 0;
    u32 fault_record_offset = 0x220;
    u32 fault_status = 0;
    u32 fault_event_control = 1u << 31;
    u32 fault_event_address = 0;
    u32 fault_event_upper_address = 0;
    u32 fault_event_data = 0;
    std::array<u32, 8> fault_event_write_offsets{};
    u32 fault_event_write_count = 0;
    u64 last_iec_descriptor = 0;
    u64 last_wait_descriptor = 0;
    u64 last_wait_status_address = 0;
    u32 status = 0;
    u32 global_command = 0;
    u32 writes = 0;
    u32 allocations = 0;
    u32 releases = 0;
    u32 flushes = 0;
    u32 queued_invalidation_descriptors_processed = 0;
    u32 queued_invalidation_context_descriptors = 0;
    u32 queued_invalidation_iotlb_descriptors = 0;
    u32 queued_invalidation_iec_descriptors = 0;
    u32 context_invalidation_writes = 0;
    u32 iotlb_invalidation_writes = 0;
    u64 last_context_descriptor = 0;
    u64 last_iotlb_descriptor = 0;
    u32 fail_flush_on_call = 0;
    bool fail_flush = false;
    bool complete_commands = true;
    bool fail_status_read_after_root_pointer = false;
    bool fail_status_read = false;
    bool fail_context_command_write = false;
    bool fail_context_read_after_write = false;
    bool fail_iotlb_read_after_write = false;
    bool complete_queued_invalidation_enable = true;
    bool complete_interrupt_table_pointer = true;
    bool complete_interrupt_remapping_enable = true;
    bool complete_queued_invalidation_wait = true;
    bool fail_fault_record_read = false;
    bool fail_fault_event_data_write = false;
    bool fail_fault_event_unmask_write = false;
};

void *resolve_page(void *context, u64 physical_address);

bool read_vtd32(void *context, u64, u32 offset, u32 &value)
{
    auto &fake = *static_cast<fake_vtd *>(context);
    if (offset == 0)
    {
        value = 0x10;
        return true;
    }
    if (offset == global_status_offset)
    {
        if (fake.fail_status_read)
            return false;
        value = fake.status;
        return true;
    }
    if (offset == fault_status_offset)
    {
        value = fake.fault_status;
        return true;
    }
    if (offset == fault_event_control_offset)
    {
        value = fake.fault_event_control;
        return true;
    }
    if (offset == fault_event_data_offset)
    {
        value = fake.fault_event_data;
        return true;
    }
    if (offset == fault_event_address_offset)
    {
        value = fake.fault_event_address;
        return true;
    }
    if (offset == fault_event_upper_address_offset)
    {
        value = fake.fault_event_upper_address;
        return true;
    }
    if (offset == fake.fault_record_offset + 12)
    {
        if (fake.fail_fault_record_read)
            return false;
        value = static_cast<u32>(fake.fault_record_high >> 32);
        return true;
    }
    return false;
}

bool write_vtd32(void *context, u64, u32 offset, u32 value)
{
    auto &fake = *static_cast<fake_vtd *>(context);
    if (offset == fault_status_offset)
    {
        fake.fault_status &= ~value;
        return true;
    }
    if (offset == fault_event_control_offset || offset == fault_event_data_offset ||
        offset == fault_event_address_offset || offset == fault_event_upper_address_offset)
    {
        if (fake.fault_event_write_count < fake.fault_event_write_offsets.size())
            fake.fault_event_write_offsets[fake.fault_event_write_count++] = offset;
        if (offset == fault_event_data_offset)
        {
            if (fake.fail_fault_event_data_write)
                return false;
            fake.fault_event_data = value;
            return true;
        }
        if (offset == fault_event_address_offset)
        {
            fake.fault_event_address = value;
            return true;
        }
        if (offset == fault_event_upper_address_offset)
        {
            fake.fault_event_upper_address = value;
            return true;
        }

        if ((value & (1u << 31)) == 0 && fake.fail_fault_event_unmask_write)
            return false;
        fake.fault_event_control = (value & ~(1u << 30)) | (fake.fault_event_control & (1u << 30));
        return true;
    }
    if (offset == fake.fault_record_offset + 12)
    {
        fake.fault_record_high &= ~(static_cast<u64>(value) << 32);
        return true;
    }
    if (offset != global_command_offset)
        return false;
    fake.global_command = value;
    fake.writes++;
    if (fake.fail_status_read_after_root_pointer && (value & set_root_table_pointer) != 0)
        fake.fail_status_read = true;
    if (fake.complete_commands && (value & set_root_table_pointer) != 0)
        fake.status |= root_pointer_status;
    if (fake.complete_commands && (value & translation_enable) != 0)
        fake.status |= translation_enable_status;
    if (fake.complete_interrupt_table_pointer && (value & set_interrupt_remapping_table_pointer) != 0)
        fake.status |= interrupt_remapping_table_pointer_status;
    if (fake.complete_interrupt_remapping_enable && (value & interrupt_remapping_enable) != 0)
        fake.status |= interrupt_remapping_enable_status;
    if (fake.complete_queued_invalidation_enable && (value & queued_invalidation_enable) != 0)
        fake.status |= queued_invalidation_enable_status;
    return true;
}

bool read_vtd64(void *context, u64, u32 offset, u64 &value)
{
    auto &fake = *static_cast<fake_vtd *>(context);
    if (offset == fake.fault_record_offset || offset == fake.fault_record_offset + 8)
    {
        if (fake.fail_fault_record_read)
            return false;
        value = offset == fake.fault_record_offset ? fake.fault_record_low : fake.fault_record_high;
        return true;
    }
    switch (offset)
    {
        case 0x8:
            value = fake.capability;
            return true;
        case 0x10:
            value = fake.extended_capability;
            return true;
        case root_table_pointer_offset:
            value = fake.root_table_address;
            return true;
        case queued_invalidation_head_offset:
            value = fake.queued_invalidation_head;
            return true;
        case queued_invalidation_tail_offset:
            value = fake.queued_invalidation_tail;
            return true;
        case queued_invalidation_address_offset:
            value = fake.queued_invalidation_address;
            return true;
        case interrupt_remapping_table_address_offset:
            value = fake.interrupt_remapping_table_address;
            return true;
        case context_command_offset:
            if (fake.fail_context_read_after_write && fake.context_command_written != 0)
                return false;
            value = fake.context_command;
            return true;
        case iotlb_register_offset:
            if (fake.fail_iotlb_read_after_write && fake.iotlb_command_written != 0)
                return false;
            value = fake.iotlb_command;
            return true;
        default:
            return false;
    }
}

bool write_vtd64(void *context, u64, u32 offset, u64 value)
{
    auto &fake = *static_cast<fake_vtd *>(context);
    fake.writes++;
    if ((fake.status & queued_invalidation_enable_status) != 0 &&
        (offset == context_command_offset || offset == iotlb_register_offset))
        return false;
    if (offset == context_command_offset && fake.fail_context_command_write)
        return false;
    switch (offset)
    {
        case root_table_pointer_offset:
            fake.root_table_address = value;
            return true;
        case queued_invalidation_address_offset:
            fake.queued_invalidation_address = value;
            return true;
        case queued_invalidation_tail_offset:
            fake.queued_invalidation_tail = value;
            if (fake.complete_queued_invalidation_wait)
            {
                const u64 queue_base = fake.queued_invalidation_address & ~u64{0xfff};
                auto *queue_page = static_cast<u8 *>(resolve_page(&fake, queue_base));
                if (queue_page == nullptr)
                    return false;
                while (fake.queued_invalidation_head != value)
                {
                    const u32 offset = static_cast<u32>(fake.queued_invalidation_head & 0xfff);
                    auto *descriptor = reinterpret_cast<const u64 *>(queue_page + offset);
                    const u64 low = descriptor[0];
                    const u64 high = descriptor[1];
                    const u64 type = low & 0xf;
                    fake.queued_invalidation_descriptors_processed++;
                    if (type == 1)
                    {
                        fake.queued_invalidation_context_descriptors++;
                        fake.last_context_descriptor = low;
                    }
                    else if (type == 2)
                    {
                        fake.queued_invalidation_iotlb_descriptors++;
                        fake.last_iotlb_descriptor = low;
                    }
                    else if (type == 4)
                    {
                        fake.queued_invalidation_iec_descriptors++;
                        fake.last_iec_descriptor = low;
                    }
                    else if (type == 5 && (low & (1ull << 5)) != 0)
                    {
                        const u64 status_address = high & ~u64{3};
                        fake.last_wait_descriptor = low;
                        fake.last_wait_status_address = status_address;
                        const u64 status_page_base = status_address & ~u64{0xfff};
                        auto *status_page = static_cast<u8 *>(resolve_page(&fake, status_page_base));
                        if (status_page == nullptr)
                            return false;
                        auto *status_value = reinterpret_cast<u32 *>(status_page + (status_address & 0xfff));
                        *status_value = static_cast<u32>(low >> 32);
                    }
                    fake.queued_invalidation_head = (fake.queued_invalidation_head + 16) & 0xfff;
                }
            }
            return true;
        case interrupt_remapping_table_address_offset:
            fake.interrupt_remapping_table_address = value;
            return true;
        case context_command_offset:
            fake.context_invalidation_writes++;
            fake.context_command_written = value;
            fake.context_command = fake.complete_commands ? value & ~(1ull << 63) : value;
            return true;
        case iotlb_register_offset:
            fake.iotlb_invalidation_writes++;
            fake.iotlb_command_written = value;
            fake.iotlb_command = fake.complete_commands ? value & ~(1ull << 63) : value;
            return true;
        default:
            return false;
    }
}

bool allocate_page(void *context, void *&virtual_page, u64 &physical_address)
{
    auto &fake = *static_cast<fake_vtd *>(context);
    const u32 page_index = fake.allocations;
    if (page_index > fake.additional_pages.size())
        return false;
    fake.allocations++;
    if (page_index == 0)
    {
        fake.root_page.fill(0xaa);
        virtual_page = fake.root_page.data();
    }
    else
    {
        fake.additional_pages[page_index - 1].fill(0xaa);
        virtual_page = fake.additional_pages[page_index - 1].data();
    }
    physical_address = 0x01000000 + static_cast<u64>(page_index) * 4096;
    return true;
}

void *resolve_page(void *context, u64 physical_address)
{
    auto &fake = *static_cast<fake_vtd *>(context);
    if ((physical_address & 0xfff) != 0 || physical_address < 0x01000000)
        return nullptr;
    const u64 page_index = (physical_address - 0x01000000) / 4096;
    if (page_index == 0)
        return fake.root_page.data();
    if (page_index > fake.additional_pages.size())
        return nullptr;
    return fake.additional_pages[page_index - 1].data();
}

void release_page(void *context, void *) { static_cast<fake_vtd *>(context)->releases++; }

bool flush_page(void *context, void *)
{
    auto &fake = *static_cast<fake_vtd *>(context);
    fake.flushes++;
    return !fake.fail_flush && fake.flushes != fake.fail_flush_on_call;
}

iommu::intel_vtd::register_ops operations(fake_vtd &fake, bool has_cache_flush = true)
{
    return {read_vtd32,    write_vtd32,  read_vtd64,   write_vtd64,
            allocate_page, release_page, resolve_page, has_cache_flush ? flush_page : nullptr,
            &fake};
}

iommu::intel_vtd::dmar_hardware_unit make_unit()
{
    iommu::intel_vtd::dmar_hardware_unit unit{};
    unit.register_base = 0xfed90000;
    unit.register_length = 0x1000;
    unit.segment = 0;
    return unit;
}

struct intel_vtd_conformance_fixture
{
    using domain_type = iommu::intel_vtd::device_domain;

    test_allocator allocator;
    naos_test::iommu_firmware::snapshot inventory{&allocator};
    fake_vtd hardware{};
    iommu::intel_vtd::controller backend{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    u16 route_index = 0xffff;
    bool ready = false;
    bool overflow_reported = false;

    intel_vtd_conformance_fixture(bool enable_interrupt_remapping, bool enable_fault_log)
    {
        inventory.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
        inventory.dmar_host_address_width = 47;
        auto unit = make_unit();
        unit.include_all = true;
        inventory.dmar_units.push_back(unit);
        if (enable_interrupt_remapping)
            hardware.extended_capability |= (1ull << 1) | (1ull << 3);
        if (enable_fault_log)
            hardware.capability |= 0x22ull << 24;
        ready = backend.initialize(inventory, operations(hardware), failure);
    }

    bool initialized() const { return ready && failure == iommu::intel_vtd::error::none; }
    bool default_deny_ready() const { return backend.default_deny_ready(); }

    bool unowned_requester_is_denied() const
    {
        const auto *root = reinterpret_cast<const u64 *>(hardware.root_page.data());
        return root[0x43 * 2] == 0 && root[0x43 * 2 + 1] == 0;
    }

    bool create_domain(domain_type &domain) { return backend.create_device_domain(0, 0x42, 17, 6, domain, failure); }

    bool map(domain_type &domain, u64 iova, u64 physical_address, naos_test::iommu_conformance::dma_access access)
    {
        return backend.map_device_page(domain, iova, physical_address,
                                       static_cast<iommu::intel_vtd::dma_access>(access)) ==
               iommu::intel_vtd::error::none;
    }

    u8 read_write_permissions(const domain_type &domain, u64 iova) const
    {
        auto *entries = static_cast<const u64 *>(domain.page_table.root_virtual);
        for (u32 level = 4; level > 1; --level)
        {
            const u32 shift = 12 + 9 * (level - 1);
            const u32 index = static_cast<u32>((iova >> shift) & 0x1ff);
            const u64 entry = entries[index];
            if ((entry & 1u) == 0)
                return 0;
            entries = static_cast<const u64 *>(resolve_page(const_cast<fake_vtd *>(&hardware), entry & ~u64{0xfff}));
            if (entries == nullptr)
                return 0;
        }
        return static_cast<u8>(entries[(iova >> 12) & 0x1ff] & 3u);
    }

    bool unmap(domain_type &domain, u64 iova)
    {
        return backend.unmap_device_page(domain, iova) == iommu::intel_vtd::error::none;
    }

    bool destroy_domain(domain_type &domain) { return backend.destroy_device_domain(domain, failure); }
    bool domain_valid(const domain_type &domain) const { return domain.valid; }
    bool domain_quarantined(const domain_type &domain) const { return domain.state_uncertain; }
    u32 released_page_count() const { return hardware.releases; }

    u64 invalidation_count() const
    {
        return static_cast<u64>(hardware.context_invalidation_writes) + hardware.iotlb_invalidation_writes +
               hardware.queued_invalidation_context_descriptors + hardware.queued_invalidation_iotlb_descriptors;
    }

    void make_next_invalidation_unknown() { hardware.complete_commands = false; }

    bool drain_injected_fault_overflow()
    {
        hardware.fault_status = 1u;
        std::array<iommu::intel_vtd::fault_record, iommu::intel_vtd::max_fault_record_count> records{};
        iommu::intel_vtd::fault_drain_result result{};
        const bool drained = backend.drain_fault_records_for_unit(0, records.data(), records.size(), result, failure);
        overflow_reported = result.overflow;
        return drained;
    }

    bool fault_overflow_reported() const { return overflow_reported; }
    bool interrupt_remapping_ready() const { return backend.interrupt_remapping_ready(); }
    u64 interrupt_invalidation_count() const { return hardware.queued_invalidation_iec_descriptors; }

    bool install_interrupt_route()
    {
        if (!backend.interrupt_remapping_enabled() && !backend.enable_interrupt_remapping(failure))
            return false;
        bool uncertain = false;
        return backend.install_pci_msi_irte_for_unit(0, 0, 0x1234, 0x2a, 0x45, route_index, failure, uncertain) &&
               !uncertain;
    }

    bool interrupt_route_active() const
    {
        const auto *unit = backend.unit(0);
        if (unit == nullptr || route_index == 0xffff || unit->interrupt_remapping_table_virtual == nullptr)
            return false;
        const auto *entries =
            static_cast<const iommu::intel_vtd::interrupt_remapping_entry *>(unit->interrupt_remapping_table_virtual);
        return (entries[route_index].low & 1u) != 0;
    }

    bool interrupt_remapping_active_after_install() { return backend.interrupt_remapping_enabled(); }

    bool retire_interrupt_route()
    {
        return backend.disable_interrupt_remapping_entry_for_unit(0, route_index, failure);
    }
};
} // namespace

TEST_CASE("VT-d controller errors map to stable kernel statuses", "[intel_vtd][status]")
{
    using iommu::intel_vtd::error;
    const std::array mappings{
        std::pair{error::none, NA_STATUS_OK},
        std::pair{error::invalid_register_aperture, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::register_io_failed, NA_STATUS_IO_ERROR},
        std::pair{error::invalid_capability, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::unsupported_address_width, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::cache_maintenance_unavailable, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::translation_pre_enabled, NA_STATUS_IO_ERROR},
        std::pair{error::allocation_failed, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{error::invalid_root_page, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::hardware_timeout, NA_STATUS_IO_ERROR},
        std::pair{error::invalid_inventory, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::reserved_memory_requires_domain, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::too_many_units, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{error::invalid_device_id, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::device_scope_unresolved, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::context_already_present, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::context_not_present, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::invalid_page_table, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::unsupported_page_table_width, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::domain_id_exhausted, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{error::context_domain_mismatch, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::dma_page_already_mapped, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::dma_page_not_mapped, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::domain_state_uncertain, NA_STATUS_IO_ERROR},
        std::pair{error::domain_has_mappings, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::domain_mapping_count_overflow, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{error::queued_invalidation_unsupported, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::queued_invalidation_pre_enabled, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::queued_invalidation_queue_busy, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::interrupt_remapping_unsupported, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::interrupt_remapping_pre_enabled, NA_STATUS_NOT_SUPPORTED},
        std::pair{error::interrupt_remapping_table_full, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{error::interrupt_remapping_entry_not_present, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::invalid_interrupt_route, NA_STATUS_INVALID_ARGUMENT},
        std::pair{error::invalid_fault_record, NA_STATUS_IO_ERROR},
        std::pair{error::fault_event_interrupt_pre_enabled, NA_STATUS_NOT_SUPPORTED},
    };

    for (const auto &[failure, expected] : mappings)
        CHECK(iommu::intel_vtd::to_status(failure) == expected);
}

TEST_CASE("VT-d opaque route tokens round trip and reject reserved entry bits", "[intel_vtd][route-token]")
{
    const auto token = iommu::intel_vtd::encode_route_token(0x1234, 0x5678, 0x9abc, 0xdef0, 0x1122334455667788);
    u32 unit_index = 0;
    u16 entry_index = 0;
    u16 segment = 0;
    u16 requester_id = 0;
    REQUIRE(iommu::intel_vtd::decode_route_token(token, 0x1122334455667788, unit_index, entry_index, segment,
                                                 requester_id));
    CHECK(unit_index == 0x1234);
    CHECK(entry_index == 0x5678);
    CHECK(segment == 0x9abc);
    CHECK(requester_id == 0xdef0);
    CHECK_FALSE(iommu::intel_vtd::decode_route_token(token, 1, unit_index, entry_index, segment, requester_id));

    auto reserved_bits = token;
    reserved_bits.opaque[0] |= 1ull << 16;
    CHECK_FALSE(iommu::intel_vtd::decode_route_token(reserved_bits, 0x1122334455667788, unit_index, entry_index,
                                                     segment, requester_id));
    reserved_bits = token;
    reserved_bits.opaque[1] |= 1ull << 32;
    CHECK_FALSE(iommu::intel_vtd::decode_route_token(reserved_bits, 0x1122334455667788, unit_index, entry_index,
                                                     segment, requester_id));
}

TEST_CASE("VT-d route adapter rejects stale sources before controller access", "[intel_vtd][route-adapter]")
{
    iommu::intel_vtd::controller backend;
    iommu::device_source_ref source{};
    source.unit.provider = iommu::provider_kind::intel_vtd;
    source.unit.generation = 7;
    source.unit.opaque[0] = 2;
    source.segment = 1;
    source.device_id = 0x1234;

    iommu::interrupt_route_token token{};
    iommu::msi_message message{1, 1};
    bool uncertain = true;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    CHECK_FALSE(
        iommu::intel_vtd::install_pci_msi_route(backend, source, 8, 0x2a, 0x45, token, message, uncertain, failure));
    CHECK_FALSE(token.valid());
    CHECK(message.address == 0);
    CHECK(message.data == 0);
    CHECK_FALSE(uncertain);
    CHECK(failure == iommu::intel_vtd::error::invalid_interrupt_route);

    iommu::ioapic_route_encoding route{};
    route.redirection_entry = 1;
    failure = iommu::intel_vtd::error::none;
    CHECK_FALSE(iommu::intel_vtd::install_ioapic_route(backend, source, 8, 0x2a, 0x45, false, true, route, failure));
    CHECK_FALSE(route.token.valid());
    CHECK(route.redirection_entry == 0);
    CHECK(failure == iommu::intel_vtd::error::invalid_interrupt_route);
    CHECK_FALSE(iommu::intel_vtd::route_matches_pci(token, source));
    CHECK_FALSE(iommu::intel_vtd::retire_route(backend, token, source.unit.generation, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_interrupt_route);
}

TEST_CASE("VT-d provider resolves firmware PCI and IOAPIC scopes into opaque source refs", "[intel_vtd][source]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.firmware_generation = 12;
    static_cast<iommu::intel_vtd::firmware_inventory &>(snapshot).firmware_generation = 12;
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    auto include_all = make_unit();
    include_all.include_all = true;
    include_all.flags |= 1;
    snapshot.dmar_units.push_back(include_all);

    const iommu::pci_function_location function{0, 0x42, 5, 2};
    iommu::device_source_ref source{};
    CHECK(iommu::intel_vtd::resolve_pci_source(snapshot, function, 12, nullptr, nullptr, source) ==
          iommu::resolution_result::resolved);
    CHECK(source.unit.provider == iommu::provider_kind::intel_vtd);
    CHECK(source.unit.generation == 12);
    CHECK(source.unit.opaque[0] == 0);
    CHECK(source.segment == 0);
    CHECK(source.device_id == static_cast<u16>((0x42u << 8) | (5u << 3) | 2u));
    CHECK(iommu::intel_vtd::resolve_pci_source(snapshot, function, 13, nullptr, nullptr, source) ==
          iommu::resolution_result::reject);

    naos_test::iommu_firmware::snapshot ioapic_snapshot(&allocator);
    ioapic_snapshot.firmware_generation = 12;
    static_cast<iommu::intel_vtd::firmware_inventory &>(ioapic_snapshot).firmware_generation = 12;
    ioapic_snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    auto scoped_unit = make_unit();
    scoped_unit.device_scope_start = 0;
    scoped_unit.device_scope_count = 1;
    ioapic_snapshot.dmar_units.push_back(scoped_unit);
    ioapic_snapshot.dmar_device_scopes.push_back(
        iommu::intel_vtd::dmar_device_scope{0, 1, iommu::intel_vtd::dmar_device_scope_type_ioapic, 0, 7, 0});
    ioapic_snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{6, 1});

    CHECK(iommu::intel_vtd::resolve_ioapic_source(ioapic_snapshot, 7, 1, 12, nullptr, nullptr, source) ==
          iommu::resolution_result::resolved);
    CHECK(source.unit.provider == iommu::provider_kind::intel_vtd);
    CHECK(source.unit.opaque[0] == 0);
    CHECK(source.segment == 0);
    CHECK(source.device_id == static_cast<u16>((6u << 3) | 1u));
    CHECK(iommu::intel_vtd::resolve_ioapic_source(ioapic_snapshot, 7, 2, 12, nullptr, nullptr, source) ==
          iommu::resolution_result::reject);
}

TEST_CASE("VT-d enables only a fully invalidated empty root table by default", "[intel_vtd]")
{
    fake_vtd fake;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::none);
    CHECK(state.translation_enabled);
    CHECK(state.root_table_retained);
    CHECK(state.root_table_physical == fake.root_table_address);
    CHECK(fake.root_table_address == 0x01000000);
    CHECK(fake.allocations == 1);
    CHECK(fake.releases == 0);
    CHECK(fake.context_command_written == invalidate_context);
    CHECK(fake.iotlb_command_written == invalidate_iotlb);
    CHECK((fake.context_command & (1ull << 63)) == 0);
    CHECK((fake.iotlb_command & (1ull << 63)) == 0);
    CHECK((fake.global_command & translation_enable) != 0);
    CHECK(std::all_of(fake.root_page.begin(), fake.root_page.end(), [](u8 value) { return value == 0; }));
}

TEST_CASE("VT-d drains and decodes a primary DMA fault record", "[intel_vtd][fault]")
{
    fake_vtd fake;
    fake.capability |= 0x22ull << 24;
    fake.fault_status = (1u << 0) | (1u << 1);
    fake.fault_record_low = 0x5000;
    fake.fault_record_high = (1ull << 63) | (5ull << 32) | 0x18;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    iommu::intel_vtd::fault_record record{};
    iommu::intel_vtd::fault_drain_result drained{};
    REQUIRE(iommu::intel_vtd::drain_fault_records(state, &record, 1, drained, failure));

    CHECK(failure == iommu::intel_vtd::error::none);
    CHECK(drained.status == ((1u << 0) | (1u << 1)));
    CHECK(drained.count == 1);
    CHECK_FALSE(drained.more);
    CHECK(record.source_id == 0x18);
    CHECK(record.address == 0x5000);
    CHECK(record.reason == 5);
    CHECK_FALSE(record.direction_known);
    CHECK_FALSE(record.is_read);
    CHECK((fake.fault_record_high & (1ull << 63)) == 0);
    CHECK((fake.fault_status & ((1u << 0) | (1u << 1))) == 0);
}

TEST_CASE("VT-d programs a masked xAPIC fault event before enabling delivery", "[intel_vtd][fault][irq]")
{
    fake_vtd fake;
    fake.capability |= 0x22ull << 24;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    REQUIRE(iommu::intel_vtd::configure_fault_event_interrupt(state, 0x2a, 0x55, failure));

    CHECK(failure == iommu::intel_vtd::error::none);
    CHECK(state.fault_event_interrupt_enabled);
    CHECK_FALSE(state.fault_event_interrupt_state_uncertain);
    CHECK(fake.fault_event_address == (0xfee00000u | (0x2au << 12)));
    CHECK(fake.fault_event_upper_address == 0);
    CHECK(fake.fault_event_data == 0x55);
    CHECK((fake.fault_event_control & (1u << 31)) == 0);
    REQUIRE(fake.fault_event_write_count == 4);
    CHECK(fake.fault_event_write_offsets[0] == fault_event_address_offset);
    CHECK(fake.fault_event_write_offsets[1] == fault_event_upper_address_offset);
    CHECK(fake.fault_event_write_offsets[2] == fault_event_data_offset);
    CHECK(fake.fault_event_write_offsets[3] == fault_event_control_offset);
}

TEST_CASE("VT-d rejects unrepresentable or reserved fault event routes without MMIO writes", "[intel_vtd][fault][irq]")
{
    fake_vtd fake;
    fake.capability |= 0x22ull << 24;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    CHECK_FALSE(iommu::intel_vtd::configure_fault_event_interrupt(state, 0x100, 0x55, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_interrupt_route);
    CHECK(fake.fault_event_write_count == 0);
    CHECK_FALSE(state.fault_event_interrupt_enabled);

    CHECK_FALSE(iommu::intel_vtd::configure_fault_event_interrupt(state, 0x2a, 0x20, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_interrupt_route);
    CHECK(fake.fault_event_write_count == 0);
}

TEST_CASE("VT-d leaves fault events masked when message programming fails", "[intel_vtd][fault][irq]")
{
    fake_vtd fake;
    fake.capability |= 0x22ull << 24;
    fake.fail_fault_event_data_write = true;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    CHECK_FALSE(iommu::intel_vtd::configure_fault_event_interrupt(state, 0x2a, 0x55, failure));

    CHECK(failure == iommu::intel_vtd::error::register_io_failed);
    CHECK((fake.fault_event_control & (1u << 31)) != 0);
    CHECK_FALSE(state.fault_event_interrupt_enabled);
    CHECK_FALSE(state.fault_event_interrupt_state_uncertain);
}

TEST_CASE("VT-d quarantines a fault vector when unmask completion is uncertain", "[intel_vtd][fault][irq]")
{
    fake_vtd fake;
    fake.capability |= 0x22ull << 24;
    fake.fail_fault_event_unmask_write = true;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    CHECK_FALSE(iommu::intel_vtd::configure_fault_event_interrupt(state, 0x2a, 0x55, failure));

    CHECK(failure == iommu::intel_vtd::error::register_io_failed);
    CHECK(state.fault_event_interrupt_state_uncertain);
    CHECK_FALSE(state.fault_event_interrupt_enabled);
}

TEST_CASE("VT-d refuses a fault status index outside the advertised record table", "[intel_vtd][fault]")
{
    fake_vtd fake;
    fake.capability |= 0x22ull << 24;
    fake.fault_status = (1u << 1) | (1u << 8);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    iommu::intel_vtd::fault_record record{};
    iommu::intel_vtd::fault_drain_result drained{};
    CHECK_FALSE(iommu::intel_vtd::drain_fault_records(state, &record, 1, drained, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_fault_record);
    CHECK(fake.fault_status == ((1u << 1) | (1u << 8)));
}

TEST_CASE("VT-d treats an empty fault record as the end of a batch", "[intel_vtd][fault]")
{
    fake_vtd fake;
    fake.capability |= 0x22ull << 24;
    fake.fault_status = 1u << 1;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    iommu::intel_vtd::fault_record record{};
    iommu::intel_vtd::fault_drain_result drained{};
    REQUIRE(iommu::intel_vtd::drain_fault_records(state, &record, 1, drained, failure));
    CHECK(failure == iommu::intel_vtd::error::none);
    CHECK(drained.status == (1u << 1));
    CHECK(drained.count == 0);
    CHECK_FALSE(drained.more);
    CHECK(fake.fault_status == 0);
}

TEST_CASE("VT-d retains a fault record when register access fails", "[intel_vtd][fault]")
{
    fake_vtd fake;
    fake.capability |= 0x22ull << 24;
    fake.fault_status = 1u << 1;
    fake.fault_record_high = (1ull << 63) | (5ull << 32) | 0x18;
    fake.fail_fault_record_read = true;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    iommu::intel_vtd::fault_record record{};
    iommu::intel_vtd::fault_drain_result drained{};
    CHECK_FALSE(iommu::intel_vtd::drain_fault_records(state, &record, 1, drained, failure));
    CHECK(failure == iommu::intel_vtd::error::register_io_failed);
    CHECK(fake.fault_status == (1u << 1));
    CHECK((fake.fault_record_high & (1ull << 63)) != 0);
}

TEST_CASE("VT-d leaves fault records intact when the output buffer cannot hold the table", "[intel_vtd][fault]")
{
    fake_vtd fake;
    fake.capability |= (1ull << 40) | (0x22ull << 24);
    fake.fault_status = 1u << 1;
    fake.fault_record_high = (1ull << 63) | (5ull << 32) | 0x18;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    iommu::intel_vtd::fault_record record{};
    iommu::intel_vtd::fault_drain_result drained{};
    CHECK_FALSE(iommu::intel_vtd::drain_fault_records(state, &record, 1, drained, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_fault_record);
    CHECK(fake.fault_status == (1u << 1));
    CHECK((fake.fault_record_high & (1ull << 63)) != 0);
}

TEST_CASE("VT-d refuses to replace a root table while translation is already active", "[intel_vtd]")
{
    fake_vtd fake;
    fake.status = translation_enable_status;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::translation_pre_enabled);
    CHECK(fake.allocations == 0);
    CHECK(fake.writes == 0);
}

TEST_CASE("VT-d refuses to enable translation when firmware left fault interrupts unmasked", "[intel_vtd][fault][irq]")
{
    fake_vtd fake;
    fake.fault_event_control = 0;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));

    CHECK(failure == iommu::intel_vtd::error::fault_event_interrupt_pre_enabled);
    CHECK(fake.allocations == 0);
    CHECK(fake.writes == 0);
    CHECK_FALSE(state.translation_enabled);
}

TEST_CASE("VT-d refuses to take over pre-enabled interrupt remapping", "[intel_vtd][msi]")
{
    fake_vtd fake;
    fake.status = interrupt_remapping_enable_status;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::translation_pre_enabled);
    CHECK(fake.allocations == 0);
    CHECK(fake.writes == 0);
    CHECK_FALSE(state.root_table_retained);
}

TEST_CASE("VT-d initializes QI without changing interrupt-remapping state", "[intel_vtd][qi]")
{
    fake_vtd fake;
    fake.extended_capability |= 1ull << 1;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));

    CHECK(state.queued_invalidation_enabled);
    CHECK_FALSE(state.queued_invalidation_state_uncertain);
    CHECK(state.queued_invalidation_storage_retained);
    CHECK(state.queued_invalidation_queue_physical == 0x01001000);
    CHECK(state.queued_invalidation_status_physical == 0x01002000);
    CHECK(fake.queued_invalidation_address == 0x01001000);
    CHECK((fake.global_command & (translation_enable | queued_invalidation_enable)) ==
          (translation_enable | queued_invalidation_enable));
    CHECK((fake.status & queued_invalidation_enable_status) != 0);
    CHECK((fake.status & interrupt_remapping_enable_status) == 0);
    CHECK(fake.allocations == 3);
    CHECK(fake.releases == 0);
}

TEST_CASE("VT-d leaves QI unavailable when hardware does not advertise it", "[intel_vtd][qi]")
{
    fake_vtd fake;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    const u32 writes_before = fake.writes;
    CHECK_FALSE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));

    CHECK(failure == iommu::intel_vtd::error::queued_invalidation_unsupported);
    CHECK(fake.allocations == 1);
    CHECK(fake.writes == writes_before);
    CHECK_FALSE(state.queued_invalidation_enabled);
}

TEST_CASE("VT-d retains QI pages when QI enable completion is uncertain", "[intel_vtd][qi]")
{
    fake_vtd fake;
    fake.extended_capability |= 1ull << 1;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    fake.complete_queued_invalidation_enable = false;
    CHECK_FALSE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));

    CHECK(failure == iommu::intel_vtd::error::hardware_timeout);
    CHECK(state.queued_invalidation_state_uncertain);
    CHECK(state.queued_invalidation_storage_retained);
    CHECK(state.queued_invalidation_queue_virtual != nullptr);
    CHECK(state.queued_invalidation_status_virtual != nullptr);
    CHECK(fake.allocations == 3);
    CHECK(fake.releases == 0);
}

TEST_CASE("VT-d installs an invalid IR table and completes a global IEC invalidation", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    REQUIRE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));

    CHECK(state.interrupt_remapping_table_ready);
    CHECK(state.interrupt_remapping_table_retained);
    CHECK_FALSE(state.interrupt_remapping_state_uncertain);
    CHECK(state.interrupt_remapping_table_physical == 0x01003000);
    CHECK(fake.interrupt_remapping_table_address == (0x01003000 | 7));
    CHECK((fake.status & interrupt_remapping_table_pointer_status) != 0);
    CHECK((fake.status & interrupt_remapping_enable_status) == 0);
    CHECK(fake.queued_invalidation_iec_descriptors == 1);
    CHECK(fake.last_iec_descriptor == 4);
    CHECK(fake.last_wait_descriptor == (5ull | (1ull << 5) | (1ull << 6) | (1ull << 32)));
    CHECK(fake.last_wait_status_address == 0x01002000);
    CHECK(fake.queued_invalidation_descriptors_processed == 2);
    CHECK(fake.queued_invalidation_head == 32);
    CHECK(fake.queued_invalidation_tail == 32);
    CHECK(std::all_of(fake.additional_pages[2].begin(), fake.additional_pages[2].end(),
                      [](u8 value) { return value == 0; }));
}

TEST_CASE("VT-d enables IRE only after a prepared table and preserves TE plus QIE", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    REQUIRE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));

    REQUIRE(iommu::intel_vtd::enable_interrupt_remapping(state, failure));
    CHECK(state.interrupt_remapping_enabled);
    CHECK_FALSE(state.interrupt_remapping_state_uncertain);
    CHECK((fake.global_command & (translation_enable | queued_invalidation_enable | interrupt_remapping_enable)) ==
          (translation_enable | queued_invalidation_enable | interrupt_remapping_enable));
    CHECK((fake.status & interrupt_remapping_enable_status) != 0);
}

TEST_CASE("VT-d quarantines IRE when enable completion is uncertain", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    REQUIRE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));
    fake.complete_interrupt_remapping_enable = false;

    CHECK_FALSE(iommu::intel_vtd::enable_interrupt_remapping(state, failure));
    CHECK(failure == iommu::intel_vtd::error::hardware_timeout);
    CHECK_FALSE(state.interrupt_remapping_enabled);
    CHECK(state.interrupt_remapping_state_uncertain);
    CHECK(state.interrupt_remapping_table_retained);
}

TEST_CASE("VT-d quarantines the IR table when IEC completion is uncertain", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    fake.complete_queued_invalidation_wait = false;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    CHECK_FALSE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));

    CHECK(failure == iommu::intel_vtd::error::hardware_timeout);
    CHECK(state.interrupt_remapping_table_retained);
    CHECK(state.interrupt_remapping_state_uncertain);
    CHECK_FALSE(state.interrupt_remapping_table_ready);
    CHECK(state.queued_invalidation_enabled);
    CHECK(state.queued_invalidation_state_uncertain);
    CHECK((fake.status & interrupt_remapping_enable_status) == 0);
    CHECK(fake.allocations == 4);
    CHECK(fake.releases == 0);
}

TEST_CASE("VT-d publishes PCI MSI IRTEs present-last and retires them with a fenced IEC", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    bool outcome_uncertain = false;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    REQUIRE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));

    u16 index = 0xffff;
    CHECK_FALSE(
        iommu::intel_vtd::install_pci_msi_irte(state, 0xabcd, 0x1234, 0x45, true, index, failure, outcome_uncertain));
    CHECK(failure == iommu::intel_vtd::error::interrupt_remapping_unsupported);
    CHECK_FALSE(outcome_uncertain);
    CHECK(index == 0xffff);
    CHECK(state.next_interrupt_remapping_index == 0);

    REQUIRE(
        iommu::intel_vtd::install_pci_msi_irte(state, 0xabcd, 0x2a, 0x45, false, index, failure, outcome_uncertain));
    CHECK_FALSE(outcome_uncertain);
    CHECK(index == 0);
    CHECK(state.next_interrupt_remapping_index == 1);
    CHECK(fake.queued_invalidation_iec_descriptors == 2);
    CHECK_FALSE(state.interrupt_remapping_state_uncertain);

    auto *table = reinterpret_cast<iommu::intel_vtd::interrupt_remapping_entry *>(fake.additional_pages[2].data());
    CHECK(table[0].low == (1ull | (0x45ull << 16) | (0x2aull << 40)));
    CHECK(table[0].high == ((1ull << 18) | 0xabcdull));

    REQUIRE(iommu::intel_vtd::disable_interrupt_remapping_entry(state, index, failure));
    CHECK((table[0].low & 1) == 0);
    CHECK(table[0].high == ((1ull << 18) | 0xabcdull));
    CHECK(fake.queued_invalidation_iec_descriptors == 3);
    CHECK_FALSE(iommu::intel_vtd::disable_interrupt_remapping_entry(state, index, failure));
    CHECK(failure == iommu::intel_vtd::error::interrupt_remapping_entry_not_present);

    u16 next_index = 0xffff;
    REQUIRE(iommu::intel_vtd::install_pci_msi_irte(state, 0xabcd, 0x2b, 0x46, false, next_index, failure,
                                                   outcome_uncertain));
    CHECK(next_index == 1);
}

TEST_CASE("VT-d quarantines an MSI IRTE when IEC completion is uncertain", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    bool outcome_uncertain = false;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    REQUIRE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));
    fake.complete_queued_invalidation_wait = false;

    u16 index = 0xffff;
    CHECK_FALSE(
        iommu::intel_vtd::install_pci_msi_irte(state, 0x1234, 0x2a, 0x45, false, index, failure, outcome_uncertain));
    CHECK(failure == iommu::intel_vtd::error::hardware_timeout);
    CHECK(outcome_uncertain);
    CHECK(index == 0xffff);
    CHECK(state.next_interrupt_remapping_index == 1);
    CHECK(state.interrupt_remapping_state_uncertain);
    CHECK(state.queued_invalidation_state_uncertain);

    auto *table = reinterpret_cast<iommu::intel_vtd::interrupt_remapping_entry *>(fake.additional_pages[2].data());
    CHECK((table[0].low & 1) == 1);
}

TEST_CASE("VT-d installs level-triggered IOAPIC IRTEs using the resolved source ID", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    REQUIRE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));

    u16 index = 0xffff;
    REQUIRE(iommu::intel_vtd::install_ioapic_irte(state, 0x9abc, 0x2a, 0x45, true, index, failure));
    CHECK(index == 0);
    CHECK(fake.queued_invalidation_iec_descriptors == 2);

    auto *table = reinterpret_cast<iommu::intel_vtd::interrupt_remapping_entry *>(fake.additional_pages[2].data());
    CHECK(table[0].low == (1ull | (1ull << 4) | (0x45ull << 16) | (0x2aull << 40)));
    CHECK(table[0].high == ((1ull << 18) | 0x9abcull));
}

TEST_CASE("VT-d never reuses retired MSI IRTE indices during the boot", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    bool outcome_uncertain = false;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    REQUIRE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));

    u16 first_index = 0xffff;
    REQUIRE(iommu::intel_vtd::install_pci_msi_irte(state, 0x1234, 0x2a, 0x45, false, first_index, failure,
                                                   outcome_uncertain));
    REQUIRE(iommu::intel_vtd::disable_interrupt_remapping_entry(state, first_index, failure));
    u16 second_index = 0xffff;
    REQUIRE(iommu::intel_vtd::install_pci_msi_irte(state, 0x1234, 0x2a, 0x46, false, second_index, failure,
                                                   outcome_uncertain));

    CHECK(first_index == 0);
    CHECK(second_index == 1);
    CHECK(state.next_interrupt_remapping_index == 2);
}

TEST_CASE("VT-d IR table allocation is bounded by the programmed 256-entry size", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    bool outcome_uncertain = false;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    REQUIRE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));

    for (u16 expected_index = 0; expected_index < iommu::intel_vtd::interrupt_remapping_entry_count; ++expected_index)
    {
        u16 index = 0xffff;
        REQUIRE(iommu::intel_vtd::install_pci_msi_irte(state, 0x1234, 0x2a, 0x45, false, index, failure,
                                                       outcome_uncertain));
        CHECK(index == expected_index);
    }

    u16 exhausted_index = 0;
    CHECK_FALSE(iommu::intel_vtd::install_pci_msi_irte(state, 0x1234, 0x2a, 0x45, false, exhausted_index, failure,
                                                       outcome_uncertain));
    CHECK(failure == iommu::intel_vtd::error::interrupt_remapping_table_full);
    CHECK(exhausted_index == 0xffff);
    CHECK(state.next_interrupt_remapping_index == iommu::intel_vtd::interrupt_remapping_entry_count);
}

TEST_CASE("VT-d does not allocate an IR table when interrupt remapping is unsupported", "[intel_vtd][ir]")
{
    fake_vtd fake;
    fake.extended_capability |= 1ull << 1;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    REQUIRE(iommu::intel_vtd::initialize_queued_invalidation(state, failure));
    CHECK_FALSE(iommu::intel_vtd::prepare_interrupt_remapping(state, failure));

    CHECK(failure == iommu::intel_vtd::error::interrupt_remapping_unsupported);
    CHECK(fake.allocations == 3);
    CHECK(fake.interrupt_remapping_table_address == 0);
}

TEST_CASE("VT-d refuses apertures that cannot contain the invalidation registers", "[intel_vtd]")
{
    fake_vtd fake;
    auto io = operations(fake);
    auto unit = make_unit();
    unit.register_length = 0x100;
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(unit, 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_register_aperture);
    CHECK(fake.allocations == 0);
    CHECK(fake.writes == 0);
}

TEST_CASE("VT-d rejects non-coherent tables without a cache-flush callback", "[intel_vtd]")
{
    fake_vtd fake;
    fake.extended_capability &= ~u64{1};
    auto io = operations(fake, false);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::cache_maintenance_unavailable);
    CHECK(fake.allocations == 0);
    CHECK(fake.writes == 0);
}

TEST_CASE("VT-d retains a programmed root page when hardware completion is uncertain", "[intel_vtd]")
{
    fake_vtd fake;
    fake.complete_commands = false;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::hardware_timeout);
    CHECK(state.root_table_retained);
    CHECK(state.state_uncertain);
    CHECK(fake.releases == 0);
}

TEST_CASE("VT-d distinguishes register read failure from a hardware timeout", "[intel_vtd]")
{
    fake_vtd fake;
    fake.fail_status_read_after_root_pointer = true;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::register_io_failed);
    CHECK(state.root_table_retained);
    CHECK(state.state_uncertain);
    CHECK(fake.releases == 0);
}

TEST_CASE("VT-d reports context command write failure distinctly from timeout", "[intel_vtd]")
{
    fake_vtd fake;
    fake.fail_context_command_write = true;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::register_io_failed);
    CHECK(state.state_uncertain);
}

TEST_CASE("VT-d reports context command polling read failure distinctly from timeout", "[intel_vtd]")
{
    fake_vtd fake;
    fake.fail_context_read_after_write = true;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::register_io_failed);
    CHECK(state.state_uncertain);
}

TEST_CASE("VT-d reports IOTLB polling read failure distinctly from timeout", "[intel_vtd]")
{
    fake_vtd fake;
    fake.fail_iotlb_read_after_write = true;
    auto io = operations(fake);
    iommu::intel_vtd::unit_state state{};
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(iommu::intel_vtd::enable_default_deny(make_unit(), 47, io, state, failure));
    CHECK(failure == iommu::intel_vtd::error::register_io_failed);
    CHECK(state.state_uncertain);
}

TEST_CASE("VT-d controller activates only a validated RMRR-free inventory", "[intel_vtd]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(controller.initialize(snapshot, io, failure));
    CHECK(controller.default_deny_ready());
    CHECK(controller.unit_count() == 1);
    CHECK(controller.active_unit_count() == 1);
}

TEST_CASE("VT-d controller prepares QI but leaves interrupt remapping disabled", "[intel_vtd][qi]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(controller.initialize(snapshot, io, failure));
    CHECK(controller.default_deny_ready());
    CHECK(controller.active_unit_count() == 1);
    CHECK(controller.queued_invalidation_unit_count() == 1);
    CHECK(controller.interrupt_remapping_table_ready_unit_count() == 1);
    CHECK((fake.status & queued_invalidation_enable_status) != 0);
    CHECK((fake.status & interrupt_remapping_table_pointer_status) != 0);
    CHECK((fake.status & interrupt_remapping_enable_status) == 0);
    CHECK(fake.queued_invalidation_iec_descriptors == 1);
}

TEST_CASE("VT-d controller enables IRE only when every DMAR unit is prepared", "[intel_vtd][ir]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(controller.initialize(snapshot, io, failure));
    REQUIRE(controller.interrupt_remapping_ready());
    CHECK_FALSE(controller.interrupt_remapping_enabled());
    REQUIRE(controller.enable_interrupt_remapping(failure));
    CHECK(controller.interrupt_remapping_enabled());
    CHECK_FALSE(controller.interrupt_remapping_activation_uncertain());
    CHECK((fake.status & interrupt_remapping_enable_status) != 0);

    fake.status &= ~interrupt_remapping_enable_status;
    CHECK_FALSE(controller.interrupt_remapping_enabled());
    CHECK(controller.interrupt_remapping_activation_uncertain());
}

TEST_CASE("VT-d controller refuses IRE when any unit lacks a ready IR table", "[intel_vtd][ir]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    fake.extended_capability |= 1ull << 1;
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(controller.initialize(snapshot, io, failure));
    CHECK_FALSE(controller.interrupt_remapping_ready());
    CHECK_FALSE(controller.enable_interrupt_remapping(failure));
    CHECK(failure == iommu::intel_vtd::error::interrupt_remapping_unsupported);
    CHECK((fake.status & interrupt_remapping_enable_status) == 0);
}

TEST_CASE("VT-d controller installs an IOAPIC IRTE only in the selected segment unit", "[intel_vtd][ir]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(controller.initialize(snapshot, io, failure));
    u16 entry_index = 0xffff;
    CHECK_FALSE(controller.install_ioapic_irte_for_unit(1, 0, 0xff00, 0, 0x45, false, entry_index, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_inventory);
    CHECK(entry_index == 0xffff);

    CHECK_FALSE(controller.install_ioapic_irte_for_unit(0, 1, 0xff00, 0, 0x45, false, entry_index, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_inventory);
    CHECK(entry_index == 0xffff);

    REQUIRE(controller.install_ioapic_irte_for_unit(0, 0, 0xff00, 0, 0x45, false, entry_index, failure));
    CHECK(entry_index == 0);
    CHECK(fake.queued_invalidation_iec_descriptors == 2);
    auto *table = reinterpret_cast<iommu::intel_vtd::interrupt_remapping_entry *>(fake.additional_pages[2].data());
    CHECK(table[0].low == (1ull | (0x45ull << 16)));
    CHECK(table[0].high == ((1ull << 18) | 0xff00ull));
}

TEST_CASE("VT-d controller installs PCI MSI IRTE only after IRE is active", "[intel_vtd][ir]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    fake.extended_capability |= (1ull << 1) | (1ull << 3);
    const auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    bool outcome_uncertain = false;

    REQUIRE(controller.initialize(snapshot, io, failure));
    u16 entry_index = 0xffff;
    CHECK_FALSE(
        controller.install_pci_msi_irte_for_unit(0, 0, 0xabcd, 0x2a, 0x45, entry_index, failure, outcome_uncertain));
    CHECK(failure == iommu::intel_vtd::error::interrupt_remapping_unsupported);
    CHECK_FALSE(outcome_uncertain);
    CHECK(entry_index == 0xffff);

    CHECK_FALSE(
        controller.install_pci_msi_irte_for_unit(1, 0, 0xabcd, 0x2a, 0x45, entry_index, failure, outcome_uncertain));
    CHECK(failure == iommu::intel_vtd::error::invalid_inventory);
    CHECK(entry_index == 0xffff);

    CHECK_FALSE(
        controller.install_pci_msi_irte_for_unit(0, 1, 0xabcd, 0x2a, 0x45, entry_index, failure, outcome_uncertain));
    CHECK(failure == iommu::intel_vtd::error::invalid_inventory);
    CHECK(entry_index == 0xffff);

    REQUIRE(controller.enable_interrupt_remapping(failure));
    REQUIRE(
        controller.install_pci_msi_irte_for_unit(0, 0, 0xabcd, 0x2a, 0x45, entry_index, failure, outcome_uncertain));
    CHECK_FALSE(outcome_uncertain);
    CHECK(entry_index == 0);
    CHECK(fake.queued_invalidation_iec_descriptors == 2);
    auto *table = reinterpret_cast<iommu::intel_vtd::interrupt_remapping_entry *>(fake.additional_pages[2].data());
    CHECK(table[0].low == (1ull | (0x45ull << 16) | (0x2aull << 40)));
    CHECK(table[0].high == ((1ull << 18) | 0xabcdull));

    fake.complete_queued_invalidation_wait = false;
    entry_index = 0xffff;
    CHECK_FALSE(
        controller.install_pci_msi_irte_for_unit(0, 0, 0xabcd, 0x2a, 0x46, entry_index, failure, outcome_uncertain));
    CHECK(failure == iommu::intel_vtd::error::hardware_timeout);
    CHECK(outcome_uncertain);
    CHECK(entry_index == 0xffff);
    CHECK((table[1].low & 1) == 1);
}

TEST_CASE("VT-d controller keeps DMA default-deny when QI initialization is uncertain", "[intel_vtd][qi]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    fake.extended_capability |= 1ull << 1;
    fake.complete_queued_invalidation_enable = false;
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    REQUIRE(controller.initialize(snapshot, io, failure));
    CHECK(controller.default_deny_ready());
    CHECK(controller.active_unit_count() == 1);
    CHECK(controller.queued_invalidation_unit_count() == 0);
    CHECK(fake.allocations == 3);
    CHECK(fake.releases == 0);
}

TEST_CASE("VT-d controller leaves translation disabled until every RMRR has a domain mapping", "[intel_vtd]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());
    snapshot.dmar_reserved_regions.push_back(iommu::intel_vtd::dmar_reserved_region{0x80000000, 0x1000, 0, 1, 0});

    fake_vtd fake;
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;

    CHECK_FALSE(controller.initialize(snapshot, io, failure));
    CHECK(failure == iommu::intel_vtd::error::reserved_memory_requires_domain);
    CHECK_FALSE(controller.default_deny_ready());
    CHECK(fake.allocations == 0);
    CHECK(fake.writes == 0);
}

TEST_CASE("VT-d installs a bounded device context before granting a second-level domain", "[intel_vtd][dma]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    auto unit = make_unit();
    unit.include_all = true;
    snapshot.dmar_units.push_back(unit);

    fake_vtd fake;
    fake.extended_capability &= ~u64{1};
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));

    u16 domain_id = 0;
    CHECK(fake.flushes == 1);
    CHECK_FALSE(controller.install_device_context(0, 0x42, 17, 6, 0x02000001, 48, false, domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_page_table);
    CHECK_FALSE(controller.install_device_context(0, 0x42, 17, 6, 0x02000000, 39, false, domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::unsupported_page_table_width);
    CHECK(fake.allocations == 1);

    REQUIRE(controller.install_device_context(0, 0x42, 17, 6, 0x02000000, 48, false, domain_id, failure));
    CHECK(domain_id == 2);
    CHECK(failure == iommu::intel_vtd::error::none);
    CHECK(fake.allocations == 2);
    CHECK(fake.flushes == 4);

    auto *root = reinterpret_cast<const u64 *>(fake.root_page.data());
    const u64 context_table_physical = 0x01001000;
    CHECK(root[0x42 * 2] == (context_table_physical | 1));
    CHECK(root[0x42 * 2 + 1] == 0);

    const auto *context = reinterpret_cast<const u64 *>(fake.additional_pages[0].data());
    const u32 context_index = 17 * 8 + 6;
    CHECK(context[context_index * 2] == (0x02000000 | 1));
    CHECK(context[context_index * 2 + 1] == (2 | (static_cast<u64>(domain_id) << 8)));
    CHECK(fake.context_command_written == invalidate_context);
    CHECK(fake.iotlb_command_written == invalidate_iotlb);

    u16 second_domain_id = 0;
    REQUIRE(controller.install_device_context(0, 0x43, 1, 0, 0x02001000, 48, false, second_domain_id, failure));
    CHECK(second_domain_id != domain_id);
    REQUIRE(controller.remove_device_context(0, 0x43, 1, 0, second_domain_id, failure));

    u16 duplicate_domain_id = 0;
    CHECK_FALSE(controller.install_device_context(0, 0x42, 17, 6, 0x02000000, 48, false, duplicate_domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::context_already_present);
    u16 invalid_domain_id = 0;
    CHECK_FALSE(controller.install_device_context(0, 0x42, 32, 0, 0x02000000, 48, false, invalid_domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_device_id);

    CHECK_FALSE(controller.remove_device_context(0, 0x42, 17, 6, domain_id + 1, failure));
    CHECK(failure == iommu::intel_vtd::error::context_domain_mismatch);
    CHECK(context[context_index * 2] != 0);

    const bool removed_context = controller.remove_device_context(0, 0x42, 17, 6, domain_id, failure);
    INFO("remove context error=" << static_cast<u32>(failure));
    REQUIRE(removed_context);
    CHECK(context[context_index * 2] == 0);
    CHECK(context[context_index * 2 + 1] == 0);
    CHECK_FALSE(controller.remove_device_context(0, 0x42, 17, 6, domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::context_not_present);

    u16 replacement_domain_id = 0;
    REQUIRE(controller.install_device_context(0, 0x42, 17, 6, 0x02001000, 48, false, replacement_domain_id, failure));
    CHECK(replacement_domain_id != domain_id);
}

TEST_CASE("VT-d refuses device contexts when DMAR device-unit routing is ambiguous", "[intel_vtd][dma]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));

    u16 domain_id = 0;
    CHECK_FALSE(controller.install_device_context(0, 0, 0, 0, 0x02000000, 48, true, domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::device_scope_unresolved);
    CHECK(fake.allocations == 1);
}

TEST_CASE("VT-d retains an installed device context when invalidation completion is uncertain", "[intel_vtd][dma]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    auto unit = make_unit();
    unit.include_all = true;
    snapshot.dmar_units.push_back(unit);

    fake_vtd fake;
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));

    fake.fail_context_command_write = true;
    u16 domain_id = 0;
    CHECK_FALSE(controller.install_device_context(0, 0x42, 17, 6, 0x02000000, 48, true, domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::register_io_failed);
    CHECK(domain_id == 2);
    CHECK(fake.releases == 0);

    u16 rejected_domain_id = 0;
    CHECK_FALSE(controller.install_device_context(0, 0x42, 17, 7, 0x02001000, 48, true, rejected_domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_inventory);
}

TEST_CASE("VT-d retains a published root entry when context-table cache maintenance fails", "[intel_vtd][dma]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    auto unit = make_unit();
    unit.include_all = true;
    snapshot.dmar_units.push_back(unit);

    fake_vtd fake;
    fake.extended_capability &= ~u64{1};
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));

    // Root-table initialization is flush 1, and the new context page is flush
    // 2. Fail after publishing the context-table pointer in the root entry.
    fake.fail_flush_on_call = 3;
    u16 domain_id = 0;
    CHECK_FALSE(controller.install_device_context(0, 0x42, 17, 6, 0x02000000, 48, false, domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::cache_maintenance_unavailable);
    CHECK(domain_id == 0);
    CHECK(fake.releases == 0);

    u16 rejected_domain_id = 0;
    CHECK_FALSE(controller.install_device_context(0, 0x42, 17, 7, 0x02001000, 48, false, rejected_domain_id, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_inventory);
}

TEST_CASE("VT-d device domains map and unmap pages before context teardown", "[intel_vtd][dma]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    auto unit = make_unit();
    unit.include_all = true;
    snapshot.dmar_units.push_back(unit);

    fake_vtd fake;
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));

    iommu::intel_vtd::device_domain domain{};
    REQUIRE(controller.create_device_domain(0, 0x42, 17, 6, domain, failure));
    CHECK(domain.valid);
    CHECK(domain.context_installed);
    CHECK(domain.domain_id == 2);
    CHECK(domain.page_table.address_width == 48);

    REQUIRE(controller.map_device_page(domain, 0x4000, 0x04000000, iommu::intel_vtd::dma_access::read_write) ==
            iommu::intel_vtd::error::none);
    CHECK(fake.iotlb_command_written == invalidate_iotlb);
    CHECK(controller.map_device_page(domain, 0x4000, 0x04000000, iommu::intel_vtd::dma_access::read) ==
          iommu::intel_vtd::error::dma_page_already_mapped);
    REQUIRE(controller.unmap_device_page(domain, 0x4000) == iommu::intel_vtd::error::none);

    REQUIRE(controller.destroy_device_domain(domain, failure));
    CHECK_FALSE(domain.valid);
    CHECK(fake.releases == 4);
    const auto *context = reinterpret_cast<const u64 *>(fake.additional_pages[0].data());
    CHECK(context[(17 * 8 + 6) * 2] == 0);
}

TEST_CASE("VT-d creates and tears down a domain on an explicitly selected scoped unit", "[intel_vtd][dma]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    snapshot.dmar_units.push_back(make_unit());

    fake_vtd fake;
    const auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));

    iommu::intel_vtd::device_domain domain{};
    CHECK_FALSE(controller.create_device_domain(1, 0, 0x42, 17, 6, domain, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_inventory);
    CHECK_FALSE(domain.valid);
    CHECK_FALSE(controller.create_device_domain(0, 1, 0x42, 17, 6, domain, failure));
    CHECK(failure == iommu::intel_vtd::error::invalid_inventory);

    REQUIRE(controller.create_device_domain(0, 0, 0x42, 17, 6, domain, failure));
    CHECK(domain.valid);
    CHECK(domain.unit_index == 0);
    REQUIRE(controller.destroy_device_domain(domain, failure));
    CHECK_FALSE(domain.valid);
    CHECK(fake.releases == 1);
}

TEST_CASE("VT-d uses queued invalidation for PCI domain context changes when QI is enabled", "[intel_vtd][dma][qi]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    auto unit = make_unit();
    unit.include_all = true;
    snapshot.dmar_units.push_back(unit);

    fake_vtd fake;
    fake.extended_capability |= 1ull << 1;
    const auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));
    REQUIRE((fake.status & queued_invalidation_enable_status) != 0);
    const u64 legacy_context_command = fake.context_command_written;
    const u64 legacy_iotlb_command = fake.iotlb_command_written;

    iommu::intel_vtd::device_domain domain{};
    REQUIRE(controller.create_device_domain(0, 0x42, 17, 6, domain, failure));
    CHECK(fake.context_command_written == legacy_context_command);
    CHECK(fake.iotlb_command_written == legacy_iotlb_command);
    CHECK(fake.queued_invalidation_context_descriptors == 1);
    CHECK(fake.queued_invalidation_iotlb_descriptors == 1);
    CHECK(fake.last_context_descriptor == (1ull | (1ull << 4)));
    CHECK(fake.last_iotlb_descriptor == (2ull | (1ull << 4)));

    REQUIRE(controller.map_device_page(domain, 0x4000, 0x04000000, iommu::intel_vtd::dma_access::read_write) ==
            iommu::intel_vtd::error::none);
    CHECK(fake.queued_invalidation_iotlb_descriptors == 2);
    REQUIRE(controller.unmap_device_page(domain, 0x4000) == iommu::intel_vtd::error::none);
    CHECK(fake.queued_invalidation_iotlb_descriptors == 3);

    REQUIRE(controller.destroy_device_domain(domain, failure));
    CHECK(fake.context_command_written == legacy_context_command);
    CHECK(fake.iotlb_command_written == legacy_iotlb_command);
    CHECK(fake.queued_invalidation_context_descriptors >= 2);
    CHECK(fake.queued_invalidation_iotlb_descriptors >= 4);
}

TEST_CASE("VT-d quarantines a PCI domain when queued context invalidation completion is unknown",
          "[intel_vtd][dma][qi]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    auto unit = make_unit();
    unit.include_all = true;
    snapshot.dmar_units.push_back(unit);

    fake_vtd fake;
    fake.extended_capability |= 1ull << 1;
    const auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));
    fake.complete_queued_invalidation_wait = false;

    iommu::intel_vtd::device_domain domain{};
    CHECK_FALSE(controller.create_device_domain(0, 0x42, 17, 6, domain, failure));
    CHECK(failure == iommu::intel_vtd::error::hardware_timeout);
    CHECK(domain.valid);
    CHECK(domain.context_installed);
    CHECK(domain.state_uncertain);
    CHECK(domain.page_table.root_virtual != nullptr);
    CHECK(fake.releases == 0);
}

TEST_CASE("VT-d device domains retain mapped tables when IOTLB invalidation is uncertain", "[intel_vtd][dma]")
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    snapshot.dmar_host_address_width = 47;
    auto unit = make_unit();
    unit.include_all = true;
    snapshot.dmar_units.push_back(unit);

    fake_vtd fake;
    auto io = operations(fake);
    iommu::intel_vtd::controller controller;
    iommu::intel_vtd::error failure = iommu::intel_vtd::error::none;
    REQUIRE(controller.initialize(snapshot, io, failure));

    iommu::intel_vtd::device_domain domain{};
    REQUIRE(controller.create_device_domain(0, 0x42, 17, 6, domain, failure));
    fake.fail_iotlb_read_after_write = true;

    CHECK(controller.map_device_page(domain, 0x4000, 0x04000000, iommu::intel_vtd::dma_access::read_write) ==
          iommu::intel_vtd::error::register_io_failed);
    CHECK(domain.state_uncertain);
    CHECK_FALSE(controller.destroy_device_domain(domain, failure));
    CHECK(failure == iommu::intel_vtd::error::domain_state_uncertain);
    CHECK(domain.page_table.root_virtual != nullptr);
    CHECK(fake.releases == 0);
}

TEST_CASE("Intel VT-d passes the shared IOMMU backend conformance suite", "[iommu][conformance][intel_vtd]")
{
    naos_test::iommu_conformance::run_backend_conformance<intel_vtd_conformance_fixture>();
}
