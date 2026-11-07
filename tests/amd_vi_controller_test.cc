#include <catch2/catch_test_macros.hpp>

#include "iommu_backend_conformance.hpp"
#include "iommu_firmware_test_support.hpp"
#include "kernel/iommu/amd_vi.hpp"
#include "kernel/mm/new.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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
constexpr u64 register_base = 0xfed90000;
constexpr u32 command_tail_offset = 0x2008;
constexpr u32 status_offset = 0x2020;
constexpr u64 status_event_overflow = 1ull << 0;
constexpr u64 status_completion_wait = 1ull << 2;
constexpr u64 status_event_log_running = 1ull << 3;
constexpr u64 status_command_buffer_running = 1ull << 4;
constexpr u64 invalidate_device_table_entry_opcode = 0x02;
constexpr u64 invalidate_interrupt_table_opcode = 0x05;
constexpr u64 invalidate_all_opcode = 0x08;
constexpr u64 completion_wait_opcode = 0x01;
constexpr u64 interrupt_table_root_pointer_mask = 0x000fffffffffffc0ull;

class test_allocator final : public freelibcxx::Allocator
{
  public:
    void *allocate(size_t size, size_t) noexcept override { return std::malloc(size); }
    void deallocate(void *pointer) noexcept override { std::free(pointer); }
};

struct allocation
{
    void *virtual_address = nullptr;
    u64 physical_address = 0;
    u32 bytes = 0;
};

struct pci_iommu_capability_fixture
{
    u32 identity = 0;
    u32 capability_header = 0;
    u32 misc_info = 0;
    u16 segment = 0;
    u8 bus = 0;
    u8 device = 0;
    u8 function = 0;
    u16 ivhd_capability_offset = 0;
    u16 capability_offset = 0;
    u32 calls = 0;
    bool readable = true;
};

bool read_pci_config32(void *context, u16 segment, u8 bus, u8 device, u8 function, u16 offset, u32 &value)
{
    auto &fixture = *static_cast<pci_iommu_capability_fixture *>(context);
    fixture.segment = segment;
    fixture.bus = bus;
    fixture.device = device;
    fixture.function = function;
    fixture.capability_offset = offset;
    fixture.calls++;
    if (!fixture.readable)
        return false;
    if (offset == 0)
        value = fixture.identity;
    else if (offset == fixture.ivhd_capability_offset)
        value = fixture.capability_header;
    else if (offset == fixture.ivhd_capability_offset + 0x10)
        value = fixture.misc_info;
    else
        return false;
    return true;
}

struct fake_amd_vi
{
    std::array<allocation, 32> allocations{};
    std::array<u64, 64> submitted_commands{};
    std::array<u64, 64> dte_words_at_commands{};
    std::array<u32, 16> dte_flush_sizes{};
    u64 control = 0;
    u64 status = 0;
    u64 command_head = 0;
    u64 command_tail = 0;
    u64 event_head = 0;
    u64 event_tail = 0;
    u64 interrupt_features = 0;
    u64 next_physical_address = 0x10000000;
    u32 allocation_count = 0;
    u32 release_count = 0;
    u32 flush_count = 0;
    u32 submitted_command_count = 0;
    u32 dte_flush_count = 0;
    u32 command_buffer_bytes = 0;
    u16 tracked_device_id = 0x1234;
    void *command_buffer = nullptr;
    void *device_table = nullptr;
    bool fail_command_tail_write = false;
};

bool read_register(void *context, u64, u32 offset, u64 &value)
{
    auto &fake = *static_cast<fake_amd_vi *>(context);
    switch (offset)
    {
        case 0x18:
            value = fake.control;
            return true;
        case 0x2000:
            value = fake.command_head;
            return true;
        case 0x2008:
            value = fake.command_tail;
            return true;
        case 0x01a0:
            value = fake.interrupt_features;
            return true;
        case 0x2010:
            value = fake.event_head;
            return true;
        case 0x2018:
            value = fake.event_tail;
            return true;
        case status_offset:
            value = fake.status;
            return true;
        default:
            value = 0;
            return true;
    }
}

bool write_register(void *context, u64, u32 offset, u64 value)
{
    auto &fake = *static_cast<fake_amd_vi *>(context);
    if (offset == 0x18)
    {
        fake.control = value;
        fake.status |= status_event_log_running | status_command_buffer_running;
        return true;
    }
    if (offset == status_offset)
    {
        fake.status &= ~value;
        return true;
    }
    if (offset == 0x2010)
    {
        fake.event_head = value;
        return true;
    }
    if (offset != command_tail_offset)
        return true;

    if (fake.fail_command_tail_write)
        return false;
    if (fake.command_buffer == nullptr || fake.command_buffer_bytes == 0)
        return false;

    const auto *ring = static_cast<const iommu::amd_vi::command_entry *>(fake.command_buffer);
    u32 cursor = static_cast<u32>(fake.command_head);
    const u32 end = static_cast<u32>(value);
    while (cursor != end)
    {
        if (fake.submitted_command_count == fake.submitted_commands.size())
            return false;
        const auto &entry = ring[cursor / sizeof(iommu::amd_vi::command_entry)];
        const u32 command_index = fake.submitted_command_count++;
        fake.submitted_commands[command_index] = entry.words[0] >> 60;
        const auto *device_entries = static_cast<const iommu::amd_vi::device_table_entry *>(fake.device_table);
        fake.dte_words_at_commands[command_index] =
            device_entries == nullptr ? 0 : device_entries[fake.tracked_device_id].words[2];
        cursor = (cursor + sizeof(iommu::amd_vi::command_entry)) % fake.command_buffer_bytes;
    }
    fake.command_head = value;
    fake.command_tail = value;
    fake.status |= status_completion_wait;
    return true;
}

u64 aperture_length(void *, u64 base) { return base == register_base ? 16 * 1024 : 0; }

bool allocate_contiguous(void *context, u32 bytes, void *&virtual_address, u64 &physical_address)
{
    auto &fake = *static_cast<fake_amd_vi *>(context);
    virtual_address = nullptr;
    physical_address = 0;
    if (fake.allocation_count == fake.allocations.size() || bytes == 0 || (bytes & 0xfff) != 0)
        return false;

    void *buffer = std::aligned_alloc(4096, bytes);
    if (buffer == nullptr)
        return false;
    std::memset(buffer, 0, bytes);

    auto &entry = fake.allocations[fake.allocation_count++];
    entry = {buffer, fake.next_physical_address, bytes};
    fake.next_physical_address += bytes;
    if (fake.device_table == nullptr && bytes == iommu::amd_vi::full_device_table_bytes)
        fake.device_table = buffer;
    if (fake.command_buffer == nullptr && bytes == 4096)
    {
        fake.command_buffer = buffer;
        fake.command_buffer_bytes = bytes;
    }
    virtual_address = buffer;
    physical_address = entry.physical_address;
    return true;
}

void release_contiguous(void *context, void *virtual_address, u32)
{
    auto &fake = *static_cast<fake_amd_vi *>(context);
    for (auto &entry : fake.allocations)
    {
        if (entry.virtual_address != virtual_address)
            continue;
        std::free(entry.virtual_address);
        entry = {};
        fake.release_count++;
        return;
    }
}

bool flush_range(void *context, void *virtual_address, u32 bytes)
{
    auto &fake = *static_cast<fake_amd_vi *>(context);
    fake.flush_count++;
    if (fake.device_table != nullptr && bytes <= sizeof(u64) && fake.dte_flush_count < fake.dte_flush_sizes.size())
    {
        const auto start = reinterpret_cast<std::uintptr_t>(fake.device_table);
        const auto address = reinterpret_cast<std::uintptr_t>(virtual_address);
        const auto end = start + iommu::amd_vi::full_device_table_bytes;
        if (address >= start && address <= end && bytes <= end - address)
            fake.dte_flush_sizes[fake.dte_flush_count++] = bytes;
    }
    return true;
}

bool allocate_page(void *context, void *&virtual_page, u64 &physical_address)
{
    return allocate_contiguous(context, iommu::amd_vi::page_size, virtual_page, physical_address);
}

void release_page(void *context, void *virtual_page)
{
    release_contiguous(context, virtual_page, iommu::amd_vi::page_size);
}

void *physical_to_virtual(void *context, u64 physical_address)
{
    const auto &fake = *static_cast<const fake_amd_vi *>(context);
    for (const auto &entry : fake.allocations)
    {
        if (entry.virtual_address != nullptr && entry.physical_address == physical_address)
            return entry.virtual_address;
    }
    return nullptr;
}

bool flush_page(void *context, void *virtual_page)
{
    return flush_range(context, virtual_page, iommu::amd_vi::page_size);
}

iommu::amd_vi::page_table_ops make_page_operations(fake_amd_vi &fake)
{
    return {allocate_page, release_page, physical_to_virtual, flush_page, &fake};
}

u64 page_table_leaf_entry(const fake_amd_vi &fake, const iommu::amd_vi::page_table &table, u64 iova)
{
    auto *entries = static_cast<const u64 *>(table.root_virtual);
    for (u32 level = iommu::amd_vi::default_page_table_levels; level > 1; --level)
    {
        const u32 shift = 12 + 9 * (level - 1);
        const u32 index = static_cast<u32>((iova >> shift) & 0x1ff);
        const u64 entry = entries[index];
        if ((entry & 1u) == 0)
            return 0;
        entries = static_cast<const u64 *>(
            physical_to_virtual(const_cast<fake_amd_vi *>(&fake), entry & iommu::amd_vi::page_address_mask));
        if (entries == nullptr)
            return 0;
    }
    return entries[(iova >> 12) & 0x1ff];
}

bool bus_master_enabled(void *, u16, u16, bool &enabled)
{
    enabled = false;
    return true;
}

bool requester_present(void *, u16 segment, u16 device_id, bool &present)
{
    present = segment == 0 && device_id == 0x1234;
    return true;
}

iommu::amd_vi::controller_ops make_operations(fake_amd_vi &fake)
{
    return {read_register,
            write_register,
            aperture_length,
            allocate_contiguous,
            release_contiguous,
            flush_range,
            bus_master_enabled,
            requester_present,
            nullptr,
            &fake};
}

struct controller_fixture
{
    test_allocator allocator;
    naos_test::iommu_firmware::snapshot inventory{&allocator};
    fake_amd_vi hardware{};
    iommu::amd_vi::controller controller{};

    explicit controller_fixture(u64 interrupt_features = 0, bool include_ioapic_source = false)
    {
        hardware.interrupt_features = interrupt_features;
        inventory.ivrs_state = iommu::amd_vi::ivrs_inventory_state::valid;
        inventory.ivrs_info = 1; // EFRSup: the IVHD EFR image is authoritative.
        iommu::amd_vi::amd_ivhd_record unit{};
        unit.register_base = register_base;
        unit.register_length = 16 * 1024;
        unit.extended_features = 1ull << 6; // INVA support.
        unit.type = 0x11;
        unit.selected = true;
        iommu::amd_vi::ioapic_interrupt_source ioapic_source{};
        if (include_ioapic_source)
        {
            constexpr u16 ioapic_device_id = 0x5678;
            constexpr u8 ioapic_id = 7;
            constexpr u8 dte_setting = 0x02;
            hardware.tracked_device_id = ioapic_device_id;
            unit.device_entry_start = 0;
            unit.device_entry_count = 1;
            iommu::amd_vi::amd_ivhd_device_entry entry{};
            entry.source_id = ioapic_device_id;
            entry.type = 0x48;
            entry.data_setting = dte_setting;
            entry.handle = ioapic_id;
            entry.variety = 1;
            inventory.amd_ivhd_device_entries.push_back(entry);
            ioapic_source = {0, 0, ioapic_device_id, ioapic_id, dte_setting};
        }
        inventory.amd_ivhd_records.push_back(unit);

        iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
        const auto preconditions = include_ioapic_source
                                       ? iommu::amd_vi::activation_preconditions{true, true, &ioapic_source, 1}
                                       : iommu::amd_vi::activation_preconditions{true, true, nullptr, 0};
        REQUIRE(controller.initialize(inventory, make_operations(hardware), preconditions, failure));
        REQUIRE(failure == iommu::amd_vi::controller_error::none);
        REQUIRE(controller.default_deny_ready());
        REQUIRE(controller.active_unit_count() == 1);
    }

    ~controller_fixture()
    {
        for (auto &entry : hardware.allocations)
        {
            std::free(entry.virtual_address);
            entry = {};
        }
    }
};

bool last_route_commands_match(fake_amd_vi &hardware, std::initializer_list<u64> expected)
{
    if (expected.size() > hardware.submitted_command_count)
        return false;
    u32 index = hardware.submitted_command_count - static_cast<u32>(expected.size());
    for (const auto command : expected)
    {
        if (hardware.submitted_commands[index++] != command)
            return false;
    }
    return true;
}

u64 last_route_dte_word(fake_amd_vi &hardware, u32 from_end)
{
    if (from_end >= hardware.submitted_command_count)
        return 0;
    return hardware.dte_words_at_commands[hardware.submitted_command_count - 1 - from_end];
}

struct amd_vi_conformance_fixture
{
    using domain_type = iommu::amd_vi::device_domain;

    controller_fixture fixture;
    iommu::amd_vi::interrupt_route route{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    bool overflow_reported = false;

    amd_vi_conformance_fixture(bool, bool)
        : fixture()
    {
    }

    bool initialized() const { return fixture.controller.default_deny_ready(); }
    bool default_deny_ready() const { return fixture.controller.default_deny_ready(); }

    bool unowned_requester_is_denied() const
    {
        const auto *unit = fixture.controller.unit(0);
        if (unit == nullptr || unit->device_table_virtual == nullptr)
            return false;
        const auto *entries = static_cast<const iommu::amd_vi::device_table_entry *>(unit->device_table_virtual);
        iommu::amd_vi::device_table_entry expected{};
        return iommu::amd_vi::encode_default_deny_device_table_entry(iommu::amd_vi::default_deny_domain_id, expected) &&
               entries[0x1235].words[0] == expected.words[0] && entries[0x1235].words[1] == expected.words[1] &&
               entries[0x1235].words[2] == expected.words[2] && entries[0x1235].words[3] == expected.words[3];
    }

    bool create_domain(domain_type &domain)
    {
        return fixture.controller.create_device_domain(0, fixture.hardware.tracked_device_id, 48,
                                                       make_page_operations(fixture.hardware), domain, failure);
    }

    bool map(domain_type &domain, u64 iova, u64 physical_address, naos_test::iommu_conformance::dma_access access)
    {
        return fixture.controller.map_device_page(domain, iova, physical_address,
                                                  static_cast<iommu::amd_vi::dma_access>(access), failure);
    }

    u8 read_write_permissions(const domain_type &domain, u64 iova) const
    {
        const u64 leaf = page_table_leaf_entry(fixture.hardware, domain.translations, iova);
        return static_cast<u8>(((leaf >> 61) & 1u) | (((leaf >> 62) & 1u) << 1));
    }

    bool unmap(domain_type &domain, u64 iova) { return fixture.controller.unmap_device_page(domain, iova, failure); }

    bool destroy_domain(domain_type &domain) { return fixture.controller.destroy_device_domain(domain, failure); }
    bool domain_valid(const domain_type &domain) const { return domain.valid; }
    bool domain_quarantined(const domain_type &domain) const { return domain.state_uncertain; }
    u32 released_page_count() const { return fixture.hardware.release_count; }

    u64 invalidation_count() const
    {
        u64 count = 0;
        for (u32 index = 0; index < fixture.hardware.submitted_command_count; ++index)
        {
            const u64 opcode = fixture.hardware.submitted_commands[index];
            count += opcode == invalidate_device_table_entry_opcode ||
                     opcode == iommu::amd_vi::invalidate_iommu_pages_opcode || opcode == invalidate_all_opcode;
        }
        return count;
    }

    void make_next_invalidation_unknown() { fixture.hardware.fail_command_tail_write = true; }

    bool drain_injected_fault_overflow()
    {
        fixture.hardware.status |= status_event_overflow;
        iommu::amd_vi::event_record record{};
        iommu::amd_vi::event_drain_result result{};
        const bool drained = fixture.controller.drain_events(0, &record, 1, result, failure);
        overflow_reported = result.overflow;
        return !drained && result.overflow;
    }

    bool fault_overflow_reported() const { return overflow_reported; }
    bool interrupt_remapping_ready() const
    {
        const auto *unit = fixture.controller.unit(0);
        return unit != nullptr && unit->interrupt_remapping_supported;
    }

    u64 interrupt_invalidation_count() const
    {
        u64 count = 0;
        for (u32 index = 0; index < fixture.hardware.submitted_command_count; ++index)
            count += fixture.hardware.submitted_commands[index] == invalidate_interrupt_table_opcode;
        return count;
    }

    bool install_interrupt_route()
    {
        return fixture.controller.install_pci_interrupt_route(0, fixture.hardware.tracked_device_id, 0x2a, 0x45,
                                                              iommu::amd_vi::interrupt_delivery_mode::fixed, false,
                                                              route, failure);
    }

    bool interrupt_route_active() const
    {
        return route.valid() &&
               fixture.controller.pci_interrupt_route_matches(route, 0, fixture.hardware.tracked_device_id);
    }

    bool interrupt_remapping_active_after_install() const { return interrupt_route_active(); }

    bool retire_interrupt_route() { return fixture.controller.retire_interrupt_route(route, failure); }
};
} // namespace

TEST_CASE("AMD-Vi controller errors map to stable kernel statuses", "[amd_vi][status]")
{
    using iommu::amd_vi::controller_error;
    const std::array mappings{
        std::pair{controller_error::none, NA_STATUS_OK},
        std::pair{controller_error::invalid_inventory, NA_STATUS_INVALID_ARGUMENT},
        std::pair{controller_error::unsafe_activation, NA_STATUS_NOT_SUPPORTED},
        std::pair{controller_error::reserved_memory_requires_domain, NA_STATUS_NOT_SUPPORTED},
        std::pair{controller_error::too_many_units, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{controller_error::invalid_register_aperture, NA_STATUS_NOT_SUPPORTED},
        std::pair{controller_error::translation_pre_enabled, NA_STATUS_NOT_SUPPORTED},
        std::pair{controller_error::allocation_failed, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{controller_error::invalid_device_table, NA_STATUS_INVALID_ARGUMENT},
        std::pair{controller_error::cache_maintenance_unavailable, NA_STATUS_NOT_SUPPORTED},
        std::pair{controller_error::register_io_failed, NA_STATUS_IO_ERROR},
        std::pair{controller_error::hardware_timeout, NA_STATUS_IO_ERROR},
        std::pair{controller_error::invalid_command_pointer, NA_STATUS_IO_ERROR},
        std::pair{controller_error::invalid_event_pointer, NA_STATUS_IO_ERROR},
        std::pair{controller_error::event_log_overflow, NA_STATUS_IO_ERROR},
        std::pair{controller_error::domain_id_exhausted, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{controller_error::device_domain_conflict, NA_STATUS_INVALID_ARGUMENT},
        std::pair{controller_error::device_not_quiesced, NA_STATUS_ACCESS_DENIED},
        std::pair{controller_error::invalid_page_table, NA_STATUS_INVALID_ARGUMENT},
        std::pair{controller_error::page_table_operation_failed, NA_STATUS_INVALID_ARGUMENT},
        std::pair{controller_error::interrupt_remapping_unsupported, NA_STATUS_NOT_SUPPORTED},
        std::pair{controller_error::interrupt_table_exhausted, NA_STATUS_RESOURCE_EXHAUSTED},
        std::pair{controller_error::invalid_interrupt_route, NA_STATUS_INVALID_ARGUMENT},
    };

    for (const auto &[failure, expected] : mappings)
        CHECK(iommu::amd_vi::to_status(failure) == expected);
}

TEST_CASE("AMD-Vi opaque route tokens preserve requester route identity", "[amd_vi][route-token]")
{
    const iommu::amd_vi::interrupt_route route{7, 0x1234, 511, 0x12345678, true};
    const auto token = iommu::amd_vi::encode_route_token(route, 0x8877665544332211);
    iommu::amd_vi::interrupt_route decoded{};
    REQUIRE(iommu::amd_vi::decode_route_token(token, 0x8877665544332211, decoded));
    CHECK(decoded.unit_index == route.unit_index);
    CHECK(decoded.device_id == route.device_id);
    CHECK(decoded.table_index == route.table_index);
    CHECK(decoded.cookie == route.cookie);
    CHECK(decoded.ioapic_source == route.ioapic_source);
    CHECK_FALSE(iommu::amd_vi::decode_route_token(token, 1, decoded));

    auto reserved_bits = token;
    reserved_bits.opaque[0] |= 1ull << 48;
    CHECK_FALSE(iommu::amd_vi::decode_route_token(reserved_bits, 0x8877665544332211, decoded));
}

TEST_CASE("AMD-Vi route adapter rejects stale sources before controller access", "[amd_vi][route-adapter]")
{
    iommu::amd_vi::controller backend;
    iommu::device_source_ref source{};
    source.unit.provider = iommu::provider_kind::amd_vi;
    source.unit.generation = 7;
    source.unit.opaque[0] = 3;
    source.unit.opaque[1] = 2;
    source.segment = 1;
    source.device_id = 0x1234;

    iommu::interrupt_route_token token{};
    iommu::msi_message message{1, 1};
    bool uncertain = true;
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    CHECK_FALSE(
        iommu::amd_vi::install_pci_msi_route(backend, source, 8, 0x2a, 0x45, token, message, uncertain, failure));
    CHECK_FALSE(token.valid());
    CHECK(message.address == 0);
    CHECK(message.data == 0);
    CHECK_FALSE(uncertain);
    CHECK(failure == iommu::amd_vi::controller_error::invalid_interrupt_route);

    iommu::ioapic_route_encoding route{};
    route.redirection_entry = 1;
    failure = iommu::amd_vi::controller_error::none;
    CHECK_FALSE(iommu::amd_vi::install_ioapic_route(backend, source, 8, 0x2a, 0x45, false, true, route, failure));
    CHECK_FALSE(route.token.valid());
    CHECK(route.redirection_entry == 0);
    CHECK(failure == iommu::amd_vi::controller_error::invalid_interrupt_route);
    CHECK_FALSE(iommu::amd_vi::route_matches_pci(backend, token, source));
    CHECK_FALSE(iommu::amd_vi::retire_route(backend, token, source.unit.generation, failure));
    CHECK(failure == iommu::amd_vi::controller_error::invalid_interrupt_route);
}

TEST_CASE("AMD IOMMU PCI capability decoding owns PAsize validation", "[amd_vi][pci-capability]")
{
    iommu::amd_vi::amd_ivhd_record record{};
    record.segment = 3;
    record.device_id = static_cast<u16>((0x42u << 8) | (5u << 3) | 2u);
    record.capability_offset = 0x40;
    pci_iommu_capability_fixture fixture{};
    fixture.ivhd_capability_offset = record.capability_offset;
    fixture.identity = 0x1022;
    fixture.capability_header = 0x0f | (3u << 16);
    fixture.misc_info = 48u << 8;

    u8 physical_address_width = 0;
    REQUIRE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                        physical_address_width));
    CHECK(physical_address_width == 48);
    CHECK(fixture.calls == 3);
    CHECK(fixture.segment == 3);
    CHECK(fixture.bus == 0x42);
    CHECK(fixture.device == 5);
    CHECK(fixture.function == 2);
    CHECK(fixture.capability_offset == 0x50);

    record.capability_offset = 0xfec;
    fixture.ivhd_capability_offset = record.capability_offset;
    REQUIRE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                        physical_address_width));
    CHECK(fixture.capability_offset == 0xffc);

    fixture.capability_header = 0x0e | (3u << 16);
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));
    CHECK(physical_address_width == 0);
    fixture.capability_header = 0x0f | (2u << 16);
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));
    fixture.capability_header = 0x0f | (3u << 16) | (1u << 29);
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));

    fixture.capability_header = 0x0f | (3u << 16);
    fixture.identity = 0xffff;
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));
    fixture.identity = 0x1022;
    fixture.misc_info = 11u << 8;
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));
    fixture.misc_info = 53u << 8;
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));

    fixture.misc_info = 48u << 8;
    fixture.readable = false;
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));
    const u32 calls_before_bad_offset = fixture.calls;
    record.capability_offset = 0x41;
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));
    CHECK(fixture.calls == calls_before_bad_offset);
    record.capability_offset = 0xff0;
    CHECK_FALSE(iommu::amd_vi::physical_address_width_from_iommu_capability(record, read_pci_config32, &fixture,
                                                                            physical_address_width));
    CHECK(fixture.calls == calls_before_bad_offset);
}

TEST_CASE("AMD I/O page fault events distinguish memory from interrupt requests", "[amd_vi][iommu][event]")
{
    controller_fixture fixture;
    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    REQUIRE(unit->event_log_virtual != nullptr);

    constexpr u16 requester_id = 0x1234;
    auto *event_words = static_cast<u64 *>(unit->event_log_virtual);
    event_words[0] = requester_id | (2ull << 60) | (1ull << 53) | (1ull << 52); // IOPF, PR=1, I=0, RW=1
    event_words[1] = 0x5000;
    event_words[2] = requester_id | (2ull << 60) | (1ull << 51); // code=IOPF, I=1
    event_words[3] = 0xfee0'0000;
    event_words[4] = requester_id | (2ull << 60) | (1ull << 56) | (1ull << 53); // IOPF, TR=1, RW=1
    event_words[5] = 0x7000;
    fixture.hardware.event_tail = 3 * sizeof(iommu::amd_vi::command_entry);

    iommu::amd_vi::event_record records[3]{};
    iommu::amd_vi::event_drain_result result{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    REQUIRE(fixture.controller.drain_events(0, records, 3, result, failure));

    CHECK(result.count == 3);
    CHECK(failure == iommu::amd_vi::controller_error::none);
    CHECK(records[0].event_code == 0x02);
    CHECK(records[0].device_id == requester_id);
    CHECK(records[0].address == 0x5000);
    CHECK(records[0].is_write);
    CHECK(records[0].is_present);
    CHECK_FALSE(records[0].is_interrupt);
    CHECK_FALSE(records[0].is_translation);
    CHECK(records[1].event_code == 0x02);
    CHECK(records[1].device_id == requester_id);
    CHECK(records[1].address == 0xfee0'0000);
    CHECK(records[1].is_interrupt);
    CHECK_FALSE(records[1].is_write);
    CHECK_FALSE(records[1].is_present);
    CHECK(records[2].event_code == 0x02);
    CHECK(records[2].is_translation);
    CHECK_FALSE(records[2].is_present);
    CHECK(records[2].is_write);
    CHECK(fixture.hardware.event_head == fixture.hardware.event_tail);
}

TEST_CASE("AMD event-log overflow quarantines the controller", "[amd_vi][iommu][event][failure]")
{
    controller_fixture fixture;
    fixture.hardware.status |= status_event_overflow;

    iommu::amd_vi::event_record record{};
    iommu::amd_vi::event_drain_result result{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    CHECK_FALSE(fixture.controller.drain_events(0, &record, 1, result, failure));
    CHECK(result.count == 0);
    CHECK(result.overflow);
    CHECK(failure == iommu::amd_vi::controller_error::event_log_overflow);
    CHECK(fixture.controller.state_uncertain());

    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    CHECK(unit->event_log_overflowed);
    CHECK(unit->state_uncertain);
    CHECK_FALSE(fixture.controller.drain_events(0, &record, 1, result, failure));
    CHECK(result.overflow);
    CHECK(failure == iommu::amd_vi::controller_error::event_log_overflow);
}

TEST_CASE("AMD DMA domains publish and invalidate DTE and I/O page mappings", "[amd_vi][iommu][dma]")
{
    controller_fixture fixture;
    auto &hardware = fixture.hardware;
    iommu::amd_vi::device_domain domain{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;

    REQUIRE(fixture.controller.create_device_domain(0, hardware.tracked_device_id, 48, make_page_operations(hardware),
                                                    domain, failure));
    REQUIRE(domain.valid);
    REQUIRE(failure == iommu::amd_vi::controller_error::none);
    CHECK(last_route_commands_match(hardware, {invalidate_device_table_entry_opcode,
                                               iommu::amd_vi::invalidate_iommu_pages_opcode, completion_wait_opcode}));
    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    REQUIRE(unit->device_table_virtual != nullptr);
    const auto *device_entries = static_cast<const iommu::amd_vi::device_table_entry *>(unit->device_table_virtual);
    CHECK(((device_entries[hardware.tracked_device_id].words[0] >> 9) & 7) == 4);
    CHECK((device_entries[hardware.tracked_device_id].words[0] & 1u) != 0);
    CHECK((device_entries[hardware.tracked_device_id].words[1] & 0xffff) == domain.domain_id);

    constexpr u64 iova = 0x4000;
    constexpr u64 physical_address = 0x2000'0000;
    const u32 before_map = hardware.submitted_command_count;
    REQUIRE(
        fixture.controller.map_device_page(domain, iova, physical_address, iommu::amd_vi::dma_access::write, failure));
    REQUIRE(failure == iommu::amd_vi::controller_error::none);
    CHECK(hardware.submitted_command_count == before_map + 2);
    CHECK(last_route_commands_match(hardware, {iommu::amd_vi::invalidate_iommu_pages_opcode, completion_wait_opcode}));
    const u64 leaf = page_table_leaf_entry(hardware, domain.translations, iova);
    CHECK((leaf & iommu::amd_vi::page_address_mask) == physical_address);
    CHECK((leaf & (1ull << 62)) != 0);
    CHECK((leaf & (1ull << 61)) == 0);

    const u32 before_unmap = hardware.submitted_command_count;
    REQUIRE(fixture.controller.unmap_device_page(domain, iova, failure));
    REQUIRE(failure == iommu::amd_vi::controller_error::none);
    CHECK(hardware.submitted_command_count == before_unmap + 2);
    CHECK(last_route_commands_match(hardware, {iommu::amd_vi::invalidate_iommu_pages_opcode, completion_wait_opcode}));
    CHECK(page_table_leaf_entry(hardware, domain.translations, iova) == 0);

    const u32 before_missing_unmap = hardware.submitted_command_count;
    CHECK_FALSE(fixture.controller.unmap_device_page(domain, iova, failure));
    CHECK(failure == iommu::amd_vi::controller_error::page_table_operation_failed);
    CHECK(hardware.submitted_command_count == before_missing_unmap);

    const u32 releases_before_destroy = hardware.release_count;
    REQUIRE(fixture.controller.destroy_device_domain(domain, failure));
    CHECK(failure == iommu::amd_vi::controller_error::none);
    CHECK_FALSE(domain.valid);
    CHECK(hardware.release_count == releases_before_destroy + 4);
    CHECK(last_route_commands_match(hardware, {invalidate_device_table_entry_opcode,
                                               iommu::amd_vi::invalidate_iommu_pages_opcode, completion_wait_opcode}));
    iommu::amd_vi::device_table_entry expected_deny{};
    REQUIRE(
        iommu::amd_vi::encode_default_deny_device_table_entry(iommu::amd_vi::default_deny_domain_id, expected_deny));
    CHECK(device_entries[hardware.tracked_device_id].words[0] == expected_deny.words[0]);
    CHECK(device_entries[hardware.tracked_device_id].words[1] == expected_deny.words[1]);
}

TEST_CASE("AMD unknown IOTLB invalidation quarantines mapped page tables", "[amd_vi][iommu][dma][failure]")
{
    controller_fixture fixture;
    auto &hardware = fixture.hardware;
    iommu::amd_vi::device_domain domain{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    REQUIRE(fixture.controller.create_device_domain(0, hardware.tracked_device_id, 48, make_page_operations(hardware),
                                                    domain, failure));
    REQUIRE(fixture.controller.map_device_page(domain, 0x4000, 0x2000'0000, iommu::amd_vi::dma_access::write, failure));

    const u32 releases_before_unknown = hardware.release_count;
    hardware.fail_command_tail_write = true;
    CHECK_FALSE(fixture.controller.unmap_device_page(domain, 0x4000, failure));
    CHECK(failure == iommu::amd_vi::controller_error::hardware_timeout);
    CHECK(domain.state_uncertain);
    CHECK(fixture.controller.state_uncertain());
    CHECK(hardware.release_count == releases_before_unknown);
    CHECK(page_table_leaf_entry(hardware, domain.translations, 0x4000) == 0);

    CHECK_FALSE(fixture.controller.destroy_device_domain(domain, failure));
    CHECK(failure == iommu::amd_vi::controller_error::invalid_inventory);
    CHECK(hardware.release_count == releases_before_unknown);
}

TEST_CASE("AMD unknown DTE invalidation retains translations during domain teardown", "[amd_vi][iommu][dma][failure]")
{
    controller_fixture fixture;
    auto &hardware = fixture.hardware;
    iommu::amd_vi::device_domain domain{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    REQUIRE(fixture.controller.create_device_domain(0, hardware.tracked_device_id, 48, make_page_operations(hardware),
                                                    domain, failure));
    REQUIRE(fixture.controller.map_device_page(domain, 0x4000, 0x2000'0000, iommu::amd_vi::dma_access::write, failure));

    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    const auto *device_entries = static_cast<const iommu::amd_vi::device_table_entry *>(unit->device_table_virtual);
    const u32 releases_before_unknown = hardware.release_count;
    hardware.fail_command_tail_write = true;
    CHECK_FALSE(fixture.controller.destroy_device_domain(domain, failure));
    CHECK(failure == iommu::amd_vi::controller_error::hardware_timeout);
    CHECK(domain.valid);
    CHECK(domain.state_uncertain);
    CHECK(domain.translations.root_virtual != nullptr);
    CHECK(fixture.controller.state_uncertain());
    CHECK(unit->state_uncertain);
    CHECK(hardware.release_count == releases_before_unknown);
    CHECK((page_table_leaf_entry(hardware, domain.translations, 0x4000) & iommu::amd_vi::page_address_mask) ==
          0x2000'0000);

    iommu::amd_vi::device_table_entry expected_deny{};
    REQUIRE(
        iommu::amd_vi::encode_default_deny_device_table_entry(iommu::amd_vi::default_deny_domain_id, expected_deny));
    CHECK(device_entries[hardware.tracked_device_id].words[0] == expected_deny.words[0]);
    CHECK(device_entries[hardware.tracked_device_id].words[1] == expected_deny.words[1]);
}

TEST_CASE("AMD PCI MSI routes attach one requester table and retire it after the last route", "[amd_vi][iommu][msi]")
{
    controller_fixture fixture;
    auto &hardware = fixture.hardware;
    const u32 commands_after_init = hardware.submitted_command_count;
    REQUIRE(commands_after_init == 2);
    CHECK(hardware.submitted_commands[0] == invalidate_all_opcode);
    CHECK(hardware.submitted_commands[1] == completion_wait_opcode);
    CHECK((hardware.control & (3ull << 43)) == 0); // Basic 512-entry NumIntRemapMode.

    iommu::amd_vi::interrupt_route first{};
    iommu::amd_vi::interrupt_route second{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    REQUIRE(fixture.controller.install_pci_interrupt_route(
        0, 0x1234, 0x2a, 0x45, iommu::amd_vi::interrupt_delivery_mode::fixed, false, first, failure));
    REQUIRE(first.valid());
    CHECK(failure == iommu::amd_vi::controller_error::none);
    CHECK(last_route_commands_match(hardware, {invalidate_interrupt_table_opcode, completion_wait_opcode,
                                               invalidate_device_table_entry_opcode, completion_wait_opcode}));
    CHECK(((last_route_dte_word(hardware, 3) >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::abort));
    CHECK((last_route_dte_word(hardware, 3) & interrupt_table_root_pointer_mask) != 0);
    CHECK(((last_route_dte_word(hardware, 1) >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::remap));
    CHECK(hardware.dte_flush_count >= 2);
    CHECK(hardware.dte_flush_sizes[hardware.dte_flush_count - 2] == sizeof(u64));
    CHECK(hardware.dte_flush_sizes[hardware.dte_flush_count - 1] == sizeof(u64));
    CHECK(fixture.controller.pci_interrupt_route_matches(first, 0, 0x1234));

    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    auto *dte = static_cast<const iommu::amd_vi::device_table_entry *>(unit->device_table_virtual);
    const iommu::amd_vi::device_table_entry remapped_dte = dte[0x1234];
    CHECK(((remapped_dte.words[2] >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::remap));
    CHECK(((remapped_dte.words[2] >> 1) & 0xf) == iommu::amd_vi::maximum_basic_interrupt_table_length);

    REQUIRE(fixture.controller.install_pci_interrupt_route(
        0, 0x1234, 0x2a, 0x46, iommu::amd_vi::interrupt_delivery_mode::fixed, false, second, failure));
    REQUIRE(second.valid());
    CHECK(second.table_index != first.table_index);
    CHECK(second.cookie != first.cookie);
    CHECK(last_route_commands_match(hardware, {invalidate_interrupt_table_opcode, completion_wait_opcode}));
    CHECK(fixture.controller.pci_interrupt_route_matches(first, 0, 0x1234));
    CHECK(fixture.controller.pci_interrupt_route_matches(second, 0, 0x1234));

    REQUIRE(fixture.controller.retire_interrupt_route(first, failure));
    CHECK_FALSE(fixture.controller.pci_interrupt_route_matches(first, 0, 0x1234));
    CHECK(fixture.controller.pci_interrupt_route_matches(second, 0, 0x1234));
    CHECK(last_route_commands_match(hardware, {invalidate_interrupt_table_opcode, completion_wait_opcode}));

    REQUIRE(fixture.controller.retire_interrupt_route(second, failure));
    CHECK_FALSE(fixture.controller.pci_interrupt_route_matches(second, 0, 0x1234));
    CHECK(last_route_commands_match(hardware, {invalidate_device_table_entry_opcode, completion_wait_opcode,
                                               invalidate_interrupt_table_opcode, completion_wait_opcode,
                                               invalidate_interrupt_table_opcode, completion_wait_opcode,
                                               invalidate_device_table_entry_opcode, completion_wait_opcode}));
    CHECK(((last_route_dte_word(hardware, 7) >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::abort));
    CHECK((last_route_dte_word(hardware, 7) & interrupt_table_root_pointer_mask) != 0);
    CHECK(((last_route_dte_word(hardware, 5) >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::abort));
    CHECK((last_route_dte_word(hardware, 5) & interrupt_table_root_pointer_mask) != 0);
    CHECK((last_route_dte_word(hardware, 3) & interrupt_table_root_pointer_mask) == 0);
    CHECK((last_route_dte_word(hardware, 1) & interrupt_table_root_pointer_mask) == 0);
    CHECK(hardware.dte_flush_count >= 4);
    CHECK(hardware.dte_flush_sizes[hardware.dte_flush_count - 2] == sizeof(u64));
    CHECK(hardware.dte_flush_sizes[hardware.dte_flush_count - 1] == sizeof(u64));
    REQUIRE(unit->device_table_virtual != nullptr);
    const auto *retired_dte = static_cast<const iommu::amd_vi::device_table_entry *>(unit->device_table_virtual);
    iommu::amd_vi::device_table_entry expected_deny{};
    REQUIRE(
        iommu::amd_vi::encode_default_deny_device_table_entry(iommu::amd_vi::default_deny_domain_id, expected_deny));
    CHECK(retired_dte[0x1234].words[0] == expected_deny.words[0]);
    CHECK(retired_dte[0x1234].words[1] == expected_deny.words[1]);
    CHECK(retired_dte[0x1234].words[2] == expected_deny.words[2]);
    CHECK(retired_dte[0x1234].words[3] == expected_deny.words[3]);
    CHECK(hardware.release_count == 1); // The requester IR table is freed only after both invalidations.
}

TEST_CASE("AMD PCI MSI unknown command completion quarantines the unit and retains its IR table",
          "[amd_vi][iommu][msi][failure]")
{
    controller_fixture fixture;
    fixture.hardware.fail_command_tail_write = true;

    iommu::amd_vi::interrupt_route route{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    CHECK_FALSE(fixture.controller.install_pci_interrupt_route(
        0, 0x1234, 0x2a, 0x45, iommu::amd_vi::interrupt_delivery_mode::fixed, false, route, failure));
    CHECK(failure == iommu::amd_vi::controller_error::hardware_timeout);
    CHECK_FALSE(route.valid());
    CHECK(fixture.controller.state_uncertain());
    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    CHECK(unit->state_uncertain);
    CHECK(fixture.hardware.release_count == 0);
    CHECK(fixture.hardware.allocation_count == 4);
}

TEST_CASE("AMD reserved NumIntRemapSup values withhold MSI routes without disabling DMA protection",
          "[amd_vi][iommu][msi][capability]")
{
    controller_fixture fixture(2ull << 8);
    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    CHECK_FALSE(unit->interrupt_remapping_supported);
    CHECK(fixture.controller.default_deny_ready());

    iommu::amd_vi::interrupt_route route{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    CHECK_FALSE(fixture.controller.install_pci_interrupt_route(
        0, 0x1234, 0x2a, 0x45, iommu::amd_vi::interrupt_delivery_mode::fixed, false, route, failure));
    CHECK(failure == iommu::amd_vi::controller_error::interrupt_remapping_unsupported);
    CHECK_FALSE(route.valid());
    CHECK_FALSE(fixture.controller.state_uncertain());
    CHECK(fixture.hardware.allocation_count == 3);
}

TEST_CASE("AMD IOAPIC routes require an IVRS-authorized special requester", "[amd_vi][iommu][ioapic]")
{
    controller_fixture fixture;
    iommu::amd_vi::interrupt_route route{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;

    CHECK_FALSE(fixture.controller.install_ioapic_interrupt_route(0, 0x5678, 0x2a, 0x45, route, failure));
    CHECK(failure == iommu::amd_vi::controller_error::invalid_inventory);
    CHECK_FALSE(route.valid());
    CHECK_FALSE(fixture.controller.state_uncertain());
    CHECK(fixture.hardware.submitted_command_count == 2);
    CHECK(fixture.hardware.allocation_count == 3);
}

TEST_CASE("AMD IOAPIC routes remap the IVRS requester and restore pass-through after retirement",
          "[amd_vi][iommu][ioapic]")
{
    constexpr u16 ioapic_device_id = 0x5678;
    constexpr u8 ioapic_vector = 0x45;
    constexpr u8 ioapic_dte_setting = 0x02;
    controller_fixture fixture(0, true);
    auto &hardware = fixture.hardware;
    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    REQUIRE(unit->device_table_virtual != nullptr);
    auto *device_entries = static_cast<const iommu::amd_vi::device_table_entry *>(unit->device_table_virtual);

    iommu::amd_vi::device_table_entry expected_passthrough{};
    REQUIRE(iommu::amd_vi::encode_ioapic_default_deny_device_table_entry(iommu::amd_vi::default_deny_domain_id,
                                                                         ioapic_dte_setting, expected_passthrough));
    CHECK(device_entries[ioapic_device_id].words[0] == expected_passthrough.words[0]);
    CHECK(device_entries[ioapic_device_id].words[1] == expected_passthrough.words[1]);
    CHECK(device_entries[ioapic_device_id].words[2] == expected_passthrough.words[2]);
    CHECK(device_entries[ioapic_device_id].words[3] == expected_passthrough.words[3]);

    iommu::amd_vi::interrupt_route route{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    REQUIRE(
        fixture.controller.install_ioapic_interrupt_route(0, ioapic_device_id, 0x2a, ioapic_vector, route, failure));
    REQUIRE(route.valid());
    CHECK(route.ioapic_source);
    CHECK(route.table_index == ioapic_vector);
    CHECK(failure == iommu::amd_vi::controller_error::none);
    CHECK(last_route_commands_match(hardware, {invalidate_device_table_entry_opcode, completion_wait_opcode,
                                               invalidate_interrupt_table_opcode, completion_wait_opcode,
                                               invalidate_device_table_entry_opcode, completion_wait_opcode}));
    CHECK(((last_route_dte_word(hardware, 5) >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::abort));
    CHECK((last_route_dte_word(hardware, 5) & interrupt_table_root_pointer_mask) == 0);
    CHECK(((last_route_dte_word(hardware, 3) >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::abort));
    CHECK((last_route_dte_word(hardware, 3) & interrupt_table_root_pointer_mask) != 0);
    CHECK(((last_route_dte_word(hardware, 1) >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::remap));
    CHECK((last_route_dte_word(hardware, 1) & interrupt_table_root_pointer_mask) != 0);
    CHECK_FALSE(fixture.controller.pci_interrupt_route_matches(route, 0, ioapic_device_id));

    REQUIRE(fixture.controller.retire_interrupt_route(route, failure));
    CHECK(last_route_commands_match(hardware, {invalidate_device_table_entry_opcode, completion_wait_opcode,
                                               invalidate_interrupt_table_opcode, completion_wait_opcode,
                                               invalidate_interrupt_table_opcode, completion_wait_opcode,
                                               invalidate_device_table_entry_opcode, completion_wait_opcode,
                                               invalidate_device_table_entry_opcode, completion_wait_opcode}));
    CHECK(((last_route_dte_word(hardware, 9) >> 60) & 3) == static_cast<u64>(iommu::amd_vi::interrupt_control::abort));
    CHECK((last_route_dte_word(hardware, 9) & interrupt_table_root_pointer_mask) != 0);
    CHECK((last_route_dte_word(hardware, 5) & interrupt_table_root_pointer_mask) == 0);
    CHECK(((last_route_dte_word(hardware, 1) >> 60) & 3) ==
          static_cast<u64>(iommu::amd_vi::interrupt_control::passthrough));
    CHECK((last_route_dte_word(hardware, 1) & interrupt_table_root_pointer_mask) == 0);
    CHECK(device_entries[ioapic_device_id].words[0] == expected_passthrough.words[0]);
    CHECK(device_entries[ioapic_device_id].words[1] == expected_passthrough.words[1]);
    CHECK(device_entries[ioapic_device_id].words[2] == expected_passthrough.words[2]);
    CHECK(device_entries[ioapic_device_id].words[3] == expected_passthrough.words[3]);
    CHECK(hardware.release_count == 1);
    CHECK_FALSE(fixture.controller.state_uncertain());
}

TEST_CASE("AMD IOAPIC route command uncertainty quarantines the pass-through requester",
          "[amd_vi][iommu][ioapic][failure]")
{
    constexpr u16 ioapic_device_id = 0x5678;
    controller_fixture fixture(0, true);
    fixture.hardware.fail_command_tail_write = true;

    iommu::amd_vi::interrupt_route route{};
    iommu::amd_vi::controller_error failure = iommu::amd_vi::controller_error::none;
    CHECK_FALSE(fixture.controller.install_ioapic_interrupt_route(0, ioapic_device_id, 0x2a, 0x45, route, failure));
    CHECK(failure == iommu::amd_vi::controller_error::hardware_timeout);
    CHECK_FALSE(route.valid());
    CHECK(fixture.controller.state_uncertain());
    const auto *unit = fixture.controller.unit(0);
    REQUIRE(unit != nullptr);
    REQUIRE(unit->device_table_virtual != nullptr);
    const auto *device_entries = static_cast<const iommu::amd_vi::device_table_entry *>(unit->device_table_virtual);
    CHECK(((device_entries[ioapic_device_id].words[2] >> 60) & 3) ==
          static_cast<u64>(iommu::amd_vi::interrupt_control::abort));
    CHECK(fixture.hardware.release_count == 0);
    CHECK(fixture.hardware.allocation_count == 4);
}

TEST_CASE("AMD-Vi passes the shared IOMMU backend conformance suite", "[iommu][conformance][amd_vi]")
{
    naos_test::iommu_conformance::run_backend_conformance<amd_vi_conformance_fixture>();
}
