#include <catch2/catch_test_macros.hpp>

#include "iommu_firmware_test_support.hpp"
#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/arch/acpi/physical_access.hpp"
#include "kernel/iommu/firmware_inventory.hpp"
#include "kernel/mm/new.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace memory
{
MemoryAllocator *MemoryAllocatorV = nullptr;
} // namespace memory

namespace
{
std::map<void *, size_t> active_allocations;
std::vector<void *> retired_allocations;

class allocation_cleanup
{
  public:
    ~allocation_cleanup()
    {
        for (const auto &[pointer, size] : active_allocations)
        {
            (void)size;
            std::free(pointer);
        }
        active_allocations.clear();
        for (void *pointer : retired_allocations)
            std::free(pointer);
        retired_allocations.clear();
    }
};

class memory_allocator_scope
{
  public:
    explicit memory_allocator_scope(memory::MemoryAllocator *allocator)
        : previous_(memory::MemoryAllocatorV)
    {
        memory::MemoryAllocatorV = allocator;
    }

    ~memory_allocator_scope() { memory::MemoryAllocatorV = previous_; }

  private:
    memory::MemoryAllocator *previous_;
};

struct firmware_image
{
    std::map<u64, std::vector<u8>> regions;
    u64 allowed_device_base = 0;
    u64 allowed_device_length = 0;
    std::vector<std::pair<u64, u64>> additional_device_ranges;
    bool device_range_checked = false;
};

void write_u32(std::vector<u8> &bytes, size_t offset, u32 value)
{
    for (size_t index = 0; index < sizeof(value); index++)
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
}

void write_u16(std::vector<u8> &bytes, size_t offset, u16 value)
{
    bytes[offset] = static_cast<u8>(value);
    bytes[offset + 1] = static_cast<u8>(value >> 8);
}

void write_u64(std::vector<u8> &bytes, size_t offset, u64 value)
{
    write_u32(bytes, offset, static_cast<u32>(value));
    write_u32(bytes, offset + sizeof(u32), static_cast<u32>(value >> 32));
}

void finish_checksum(std::vector<u8> &bytes, size_t checksum_offset, size_t length = 0)
{
    bytes[checksum_offset] = 0;
    if (length == 0)
        length = bytes.size();
    u8 sum = 0;
    for (size_t index = 0; index < length; index++)
        sum = static_cast<u8>(sum + bytes[index]);
    bytes[checksum_offset] = static_cast<u8>(0 - sum);
}

std::vector<u8> make_sdt(const char signature[5], u32 length)
{
    std::vector<u8> bytes(length, 0);
    std::memcpy(bytes.data(), signature, 4);
    write_u32(bytes, 4, length);
    bytes[8] = 1;
    finish_checksum(bytes, 9);
    return bytes;
}

const u8 *map_firmware(void *context, u64 address, u64 length)
{
    const auto &regions = static_cast<firmware_image *>(context)->regions;
    for (const auto &[base, bytes] : regions)
    {
        if (address < base)
            continue;
        const u64 offset = address - base;
        if (offset <= bytes.size() && length <= bytes.size() - offset)
            return bytes.data() + offset;
    }
    return nullptr;
}

bool firmware_range_valid(void *context, u64 address, u64 length)
{
    return map_firmware(context, address, length) != nullptr;
}

bool firmware_device_range_valid(void *context, u64 address, u64 length)
{
    auto &image = *static_cast<firmware_image *>(context);
    image.device_range_checked = true;
    const auto contains = [address, length](u64 base, u64 range_length) {
        if (address < base || length == 0)
            return false;
        const u64 offset = address - base;
        return offset <= range_length && length <= range_length - offset;
    };
    if (contains(image.allowed_device_base, image.allowed_device_length))
        return true;
    for (const auto &[base, range_length] : image.additional_device_ranges)
    {
        if (contains(base, range_length))
            return true;
    }
    return false;
}

struct dmar_path_reader
{
    u16 expected_segment = 0;
    u8 expected_bus = 0;
    u8 expected_device = 0;
    u8 expected_function = 0;
    u8 header_type = 1;
    u8 primary_bus = 0;
    u8 secondary_bus = 0;
    u8 subordinate_bus = 0xff;
    u32 calls = 0;
    bool fail = false;
};

bool read_bridge_info(void *context, u16 segment, u8 bus, u8 device, u8 function, u8 &header_type, u8 &primary_bus,
                      u8 &secondary_bus, u8 &subordinate_bus)
{
    auto &reader = *static_cast<dmar_path_reader *>(context);
    reader.calls++;
    if (reader.fail || segment != reader.expected_segment || bus != reader.expected_bus ||
        device != reader.expected_device || function != reader.expected_function)
        return false;
    header_type = reader.header_type;
    primary_bus = reader.primary_bus;
    secondary_bus = reader.secondary_bus;
    subordinate_bus = reader.subordinate_bus;
    return true;
}

struct dmar_scope_bridge
{
    u16 segment = 0;
    u8 bus = 0;
    u8 device = 0;
    u8 function = 0;
    u8 header_type = 1;
    u8 primary_bus = 0;
    u8 secondary_bus = 0;
    u8 subordinate_bus = 0;
};

struct dmar_scope_reader
{
    std::array<dmar_scope_bridge, 4> bridges{};
    u32 bridge_count = 0;
    u32 calls = 0;
    bool fail = false;
};

bool read_scope_bridge_info(void *context, u16 segment, u8 bus, u8 device, u8 function, u8 &header_type,
                            u8 &primary_bus, u8 &secondary_bus, u8 &subordinate_bus)
{
    auto &reader = *static_cast<dmar_scope_reader *>(context);
    reader.calls++;
    if (reader.fail)
        return false;
    for (u32 index = 0; index < reader.bridge_count; ++index)
    {
        const auto &bridge = reader.bridges[index];
        if (bridge.segment == segment && bridge.bus == bus && bridge.device == device && bridge.function == function)
        {
            header_type = bridge.header_type;
            primary_bus = bridge.primary_bus;
            secondary_bus = bridge.secondary_bus;
            subordinate_bus = bridge.subordinate_bus;
            return true;
        }
    }
    return false;
}

void install_firmware(firmware_image &image)
{
    constexpr u64 rsdp_address = 0x1000;
    constexpr u64 xsdt_address = 0x2000;
    constexpr u64 dsdt_address = 0x9000;
    constexpr u64 facs_address = 0xa000;
    const std::vector<std::pair<u64, const char *>> children = {
        {0x3000, "APIC"}, {0x4000, "HPET"}, {0x5000, "MCFG"}, {0x6000, "TEST"}, {0x7000, "TEST"}, {0x8000, "FACP"},
    };

    std::vector<u8> rsdp(36, 0);
    std::memcpy(rsdp.data(), "RSD PTR ", 8);
    rsdp[15] = 2;
    write_u32(rsdp, 20, rsdp.size());
    write_u64(rsdp, 24, xsdt_address);
    finish_checksum(rsdp, 8, 20);
    finish_checksum(rsdp, 32);
    image.regions.emplace(rsdp_address, std::move(rsdp));

    std::vector<u8> xsdt = make_sdt("XSDT", 36 + children.size() * sizeof(u64));
    for (size_t index = 0; index < children.size(); index++)
        write_u64(xsdt, 36 + index * sizeof(u64), children[index].first);
    finish_checksum(xsdt, 9);
    image.regions.emplace(xsdt_address, std::move(xsdt));

    for (const auto &[address, signature] : children)
    {
        const u32 length = std::strcmp(signature, "MCFG") == 0 ? 44 : std::strcmp(signature, "FACP") == 0 ? 148 : 36;
        auto bytes = make_sdt(signature, length);
        if (std::strcmp(signature, "FACP") == 0)
        {
            write_u32(bytes, 36, facs_address);
            write_u32(bytes, 40, dsdt_address);
            finish_checksum(bytes, 9);
        }
        image.regions.emplace(address, std::move(bytes));
    }

    image.regions.emplace(dsdt_address, make_sdt("DSDT", 36));
    std::vector<u8> facs(64, 0);
    std::memcpy(facs.data(), "FACS", 4);
    write_u32(facs, 4, facs.size());
    image.additional_device_ranges.emplace_back(facs_address, facs.size());
    image.regions.emplace(facs_address, std::move(facs));
}

void append_xsdt_child(firmware_image &image, u64 address, std::vector<u8> table)
{
    auto &xsdt = image.regions.at(0x2000);
    const size_t old_length = xsdt.size();
    xsdt.resize(old_length + sizeof(u64));
    write_u64(xsdt, old_length, address);
    write_u32(xsdt, 4, static_cast<u32>(xsdt.size()));
    finish_checksum(xsdt, 9);
    image.regions.emplace(address, std::move(table));
}

std::vector<u8> make_dmar_with_include_all_drhd(u64 register_base, u16 segment)
{
    constexpr size_t dmar_header_bytes = 48;
    constexpr size_t drhd_bytes = 16;
    auto bytes = make_sdt("DMAR", dmar_header_bytes + drhd_bytes);
    bytes[36] = 47; // 48-bit host address width.
    bytes[48] = 0;  // DMA-remapping hardware unit definition.
    write_u16(bytes, 50, drhd_bytes);
    bytes[52] = 1; // INCLUDE_PCI_ALL.
    bytes[53] = 0; // One 4-KiB register page.
    write_u16(bytes, 54, segment);
    write_u64(bytes, 56, register_base);
    finish_checksum(bytes, 9);
    return bytes;
}

std::vector<u8> make_dmar_with_nonretained_scope_subtable(u64 register_base, u16 subtable_type)
{
    constexpr size_t dmar_header_bytes = 48;
    constexpr size_t drhd_bytes = 16;
    constexpr size_t scoped_subtable_bytes = 16;
    auto bytes = make_sdt("DMAR", dmar_header_bytes + drhd_bytes + scoped_subtable_bytes);
    bytes[36] = 47;

    write_u16(bytes, dmar_header_bytes, 0);
    write_u16(bytes, dmar_header_bytes + 2, drhd_bytes);
    bytes[dmar_header_bytes + 4] = 1; // INCLUDE_PCI_ALL.
    write_u16(bytes, dmar_header_bytes + 6, 0);
    write_u64(bytes, dmar_header_bytes + 8, register_base);

    const size_t scoped_subtable = dmar_header_bytes + drhd_bytes;
    write_u16(bytes, scoped_subtable, subtable_type);
    write_u16(bytes, scoped_subtable + 2, scoped_subtable_bytes);
    write_u16(bytes, scoped_subtable + 6, 0);
    const size_t scope = scoped_subtable + 8;
    bytes[scope] = iommu::intel_vtd::dmar_device_scope_type_pci_endpoint;
    bytes[scope + 1] = 8;
    bytes[scope + 6] = 3;
    bytes[scope + 7] = 1;
    finish_checksum(bytes, 9);
    return bytes;
}

std::vector<u8> make_dmar_with_include_all_drhd_and_scope(u64 register_base, u8 scope_type)
{
    constexpr size_t dmar_header_bytes = 48;
    constexpr size_t drhd_bytes = 16 + 8;
    auto bytes = make_sdt("DMAR", dmar_header_bytes + drhd_bytes);
    bytes[36] = 47;
    write_u16(bytes, 48, 0);
    write_u16(bytes, 50, drhd_bytes);
    bytes[52] = 1; // INCLUDE_PCI_ALL.
    write_u64(bytes, 56, register_base);
    bytes[64] = scope_type;
    bytes[65] = 8;
    bytes[68] = 0;
    bytes[69] = 0;
    bytes[70] = 3;
    bytes[71] = 1;
    finish_checksum(bytes, 9);
    return bytes;
}

std::vector<u8> make_dmar_with_drhd_and_rmrr(u64 register_base, u64 reserved_base)
{
    constexpr size_t dmar_header_bytes = 48;
    constexpr size_t drhd_bytes = 16 + 8;
    constexpr size_t rmrr_bytes = 24 + 8;
    auto bytes = make_sdt("DMAR", dmar_header_bytes + drhd_bytes + rmrr_bytes);
    bytes[36] = 47;
    write_u16(bytes, 48, 0);
    write_u16(bytes, 50, drhd_bytes);
    bytes[52] = 0; // Explicit endpoint scope, not INCLUDE_PCI_ALL.
    write_u64(bytes, 56, register_base);
    bytes[64] = 1;
    bytes[65] = 8;
    bytes[68] = 0;
    bytes[69] = 0;
    bytes[70] = 3;
    bytes[71] = 1;

    const size_t rmrr = dmar_header_bytes + drhd_bytes;
    write_u16(bytes, rmrr, 1);
    write_u16(bytes, rmrr + 2, rmrr_bytes);
    write_u64(bytes, rmrr + 8, reserved_base);
    write_u64(bytes, rmrr + 16, reserved_base + 0xfff);
    bytes[rmrr + 24] = 1;
    bytes[rmrr + 25] = 8;
    bytes[rmrr + 30] = 3;
    bytes[rmrr + 31] = 1;
    finish_checksum(bytes, 9);
    return bytes;
}

std::vector<u8> make_dmar_with_include_all_before_scoped_drhd(u64 register_base)
{
    constexpr size_t dmar_header_bytes = 48;
    constexpr size_t include_all_drhd_bytes = 16;
    constexpr size_t scoped_drhd_bytes = 24;
    auto bytes = make_sdt("DMAR", dmar_header_bytes + include_all_drhd_bytes + scoped_drhd_bytes);
    bytes[36] = 47;

    write_u16(bytes, dmar_header_bytes, 0);
    write_u16(bytes, dmar_header_bytes + 2, include_all_drhd_bytes);
    bytes[dmar_header_bytes + 4] = 1;
    bytes[dmar_header_bytes + 5] = 0;
    write_u16(bytes, dmar_header_bytes + 6, 0);
    write_u64(bytes, dmar_header_bytes + 8, register_base);

    const size_t scoped_drhd = dmar_header_bytes + include_all_drhd_bytes;
    write_u16(bytes, scoped_drhd, 0);
    write_u16(bytes, scoped_drhd + 2, scoped_drhd_bytes);
    bytes[scoped_drhd + 4] = 0;
    bytes[scoped_drhd + 5] = 0;
    write_u16(bytes, scoped_drhd + 6, 0);
    write_u64(bytes, scoped_drhd + 8, register_base + 0x1000);
    bytes[scoped_drhd + 16] = iommu::intel_vtd::dmar_device_scope_type_pci_endpoint;
    bytes[scoped_drhd + 17] = 8;
    bytes[scoped_drhd + 22] = 3;
    bytes[scoped_drhd + 23] = 0;

    finish_checksum(bytes, 9);
    return bytes;
}

void append_ivhd(std::vector<u8> &bytes, u8 type, u16 device_id)
{
    const size_t offset = bytes.size();
    const size_t fixed_bytes = type == 0x10 ? 24 : 40;
    bytes.resize(offset + fixed_bytes + 4, 0);
    bytes[offset] = type;
    write_u16(bytes, offset + 2, static_cast<u16>(fixed_bytes + 4));
    write_u16(bytes, offset + 4, 0x0002);
    write_u16(bytes, offset + 6, 0x0040);
    write_u64(bytes, offset + 8, 0xfed90000);
    write_u16(bytes, offset + 16, 0);
    write_u16(bytes, offset + 18, 0);
    if (type == 0x40)
        write_u64(bytes, offset + 32, 0x1122334455667788ull); // IVHD 40h EFR Register Image 2.
    bytes[offset + fixed_bytes] = 0x02;                       // Select one PCI DeviceID.
    write_u16(bytes, offset + fixed_bytes + 1, device_id);
}

void append_ivmd(std::vector<u8> &bytes, u16 device_id)
{
    const size_t offset = bytes.size();
    bytes.resize(offset + 32, 0);
    bytes[offset] = 0x21; // One device.
    write_u16(bytes, offset + 2, 32);
    write_u16(bytes, offset + 4, device_id);
    write_u16(bytes, offset + 8, 0);
    write_u64(bytes, offset + 16, 0x80000000);
    write_u64(bytes, offset + 24, 0x1000);
}

std::vector<u8> make_ivrs_compatibility_pair()
{
    auto bytes = make_sdt("IVRS", 48);
    write_u32(bytes, 36, 1); // IVinfo: EFR reporting is available.
    append_ivhd(bytes, 0x10, 0x0100);
    append_ivmd(bytes, 0x0100);
    append_ivhd(bytes, 0x11, 0x0101);
    append_ivmd(bytes, 0x0101);
    write_u32(bytes, 4, static_cast<u32>(bytes.size()));
    finish_checksum(bytes, 9);
    return bytes;
}

std::vector<u8> make_ivrs_unterminated_range()
{
    auto bytes = make_sdt("IVRS", 48);
    append_ivhd(bytes, 0x11, 0x0100);
    const size_t entry_offset = bytes.size() - 4;
    bytes[entry_offset] = 0x03; // Range start without its required end entry.
    write_u32(bytes, 4, static_cast<u32>(bytes.size()));
    finish_checksum(bytes, 9);
    return bytes;
}

std::vector<u8> make_ivrs_type40()
{
    auto bytes = make_sdt("IVRS", 48);
    write_u32(bytes, 36, 1); // IVinfo[EFRSup] is required by IVHD Type 40h.
    append_ivhd(bytes, 0x40, 0x0102);
    write_u32(bytes, 4, static_cast<u32>(bytes.size()));
    finish_checksum(bytes, 9);
    return bytes;
}
} // namespace

namespace memory
{
void *MemoryAllocator::allocate(u64 size, u64 align) noexcept
{
    (void)align;
    void *pointer = std::malloc(size);
    if (pointer != nullptr)
        active_allocations.emplace(pointer, size);
    return pointer;
}

void MemoryAllocator::deallocate(void *pointer) noexcept
{
    if (pointer == nullptr)
        return;
    const auto found = active_allocations.find(pointer);
    if (found == active_allocations.end())
        return;
    std::memset(pointer, 0xdd, found->second);
    active_allocations.erase(found);
    retired_allocations.push_back(pointer);
}
} // namespace memory

TEST_CASE("ACPI indirect FADT relations survive firmware table vector growth", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);
    const arch::acpi_boot::physical_source source{
        map_firmware, firmware_range_valid, firmware_range_valid, nullptr, &image,
    };
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::absent);
    CHECK(snapshot.tables.size() == 10);
    CHECK(snapshot.relations.size() == 8);

    naos_test::iommu_firmware::snapshot replacement(&allocator);
    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 2, replacement, failure));
    CHECK(replacement.firmware_generation > snapshot.firmware_generation);
    CHECK(replacement.pci_fabric_generation > snapshot.pci_fabric_generation);

    u64 fadt_id = 0;
    u64 dsdt_id = 0;
    u64 facs_id = 0;
    for (const auto &table : snapshot.tables)
    {
        if (table.signature == arch::acpi_boot::signature_value("FACP"))
            fadt_id = table.object_id;
        else if (table.signature == arch::acpi_boot::signature_value("DSDT"))
            dsdt_id = table.object_id;
        else if (table.signature == arch::acpi_boot::signature_value("FACS"))
        {
            facs_id = table.object_id;
            CHECK(table.physical_address == 0xa000);
            CHECK(table.length == 64);
            CHECK(table.snapshot == nullptr);
        }
    }
    REQUIRE(fadt_id != 0);
    REQUIRE(dsdt_id != 0);
    REQUIRE(facs_id != 0);

    bool saw_dsdt = false;
    bool saw_facs = false;
    for (const auto &relation : snapshot.relations)
    {
        if (relation.kind == arch::acpi_boot::relation_kind::fadt_dsdt)
        {
            saw_dsdt = relation.from == fadt_id && relation.to == dsdt_id;
        }
        if (relation.kind == arch::acpi_boot::relation_kind::fadt_facs)
        {
            saw_facs = relation.from == fadt_id && relation.to == facs_id;
        }
    }
    CHECK(saw_dsdt);
    CHECK(saw_facs);
}

TEST_CASE("MADT Local APIC overrides preserve 64-bit addresses and reject malformed entries",
          "[acpi_boot_tables][madt]")
{
    constexpr u64 high_lapic_base = 0x1'fee0'0000;
    std::vector<u8> madt(56, 0);
    std::memcpy(madt.data(), "APIC", 4);
    write_u32(madt, 36, 0xfee00000);
    madt[44] = 5;
    madt[45] = 12;
    write_u64(madt, 48, high_lapic_base);

    u64 base = 0;
    REQUIRE(arch::acpi_boot::madt_local_apic_base(madt.data(), madt.size(), 52, base));
    CHECK(base == high_lapic_base);

    madt[46] = 1;
    CHECK_FALSE(arch::acpi_boot::madt_local_apic_base(madt.data(), madt.size(), 52, base));
    madt[46] = 0;

    madt[45] = 11;
    CHECK_FALSE(arch::acpi_boot::madt_local_apic_base(madt.data(), madt.size(), 52, base));
    madt[45] = 12;

    madt.resize(68, 0);
    madt[56] = 5;
    madt[57] = 12;
    write_u64(madt, 60, high_lapic_base);
    CHECK_FALSE(arch::acpi_boot::madt_local_apic_base(madt.data(), madt.size(), 52, base));
}

TEST_CASE("FACS remains discoverable but cannot be opened as an immutable firmware object", "[acpi_boot_tables]")
{
    CHECK_FALSE(arch::acpi_boot::firmware_object_is_openable(arch::acpi_boot::signature_value("FACS")));
    CHECK(arch::acpi_boot::firmware_object_is_openable(arch::acpi_boot::signature_value("DSDT")));
    CHECK(arch::acpi_boot::firmware_object_is_openable(arch::acpi_boot::signature_value("FACP")));
    CHECK(arch::acpi_boot::firmware_object_flags(arch::acpi_boot::signature_value("FACS"), false) ==
          (arch::acpi_boot::firmware_object_flag_no_checksum | arch::acpi_boot::firmware_object_flag_mutable_state));
    CHECK(arch::acpi_boot::firmware_object_flags(arch::acpi_boot::signature_value("DSDT"), true) == 0);
}

TEST_CASE("FADT-referenced FACS with an invalid envelope rejects the firmware snapshot", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);
    write_u32(image.regions.at(0xa000), 4, 63);

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    CHECK_FALSE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(failure == arch::acpi_boot::error::invalid_length);
}

TEST_CASE("FADT-referenced FACS must be 64-byte aligned", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);

    auto facs = std::move(image.regions.at(0xa000));
    image.regions.erase(0xa000);
    image.regions.emplace(0xa001, std::move(facs));
    image.additional_device_ranges.clear();
    image.additional_device_ranges.emplace_back(0xa001, 64);
    write_u32(image.regions.at(0x8000), 36, 0xa001);
    finish_checksum(image.regions.at(0x8000), 9);

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    CHECK_FALSE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(failure == arch::acpi_boot::error::invalid_address);
}

TEST_CASE("FADT FACS reference cannot alias a different cataloged table", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);
    auto &fadt = image.regions.at(0x8000);
    write_u32(fadt, 36, 0x3000); // Existing APIC record, not a FACS.
    finish_checksum(fadt, 9);
    image.additional_device_ranges.emplace_back(0x3000, image.regions.at(0x3000).size());

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    CHECK_FALSE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(failure == arch::acpi_boot::error::invalid_header);
}

TEST_CASE("ACPI MCFG extents require device-range admission", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);

    constexpr u64 ecam_base = 0x10000000;
    constexpr u64 ecam_length = 0x10000000;
    auto mcfg = make_sdt("MCFG", 44 + 16);
    write_u64(mcfg, 44, ecam_base);
    mcfg[54] = 0;
    mcfg[55] = 0xff;
    finish_checksum(mcfg, 9);
    image.regions[0x5000] = std::move(mcfg);
    image.allowed_device_base = ecam_base;
    image.allowed_device_length = ecam_length;

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;
    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(image.device_range_checked);
    REQUIRE(snapshot.mcfg_regions.size() == 1);
    CHECK(snapshot.mcfg_regions[0].base == ecam_base);

    image.device_range_checked = false;
    image.allowed_device_base = ecam_base + ecam_length;
    naos_test::iommu_firmware::snapshot rejected_snapshot(&allocator);
    CHECK_FALSE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 2, rejected_snapshot, failure));
    CHECK(image.device_range_checked);
    CHECK(failure == arch::acpi_boot::error::invalid_address);
}

TEST_CASE("DMAR inventory admits only a validated device-backed DRHD aperture", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);

    constexpr u64 register_base = 0xfed90000;
    image.additional_device_ranges.emplace_back(register_base, 0x1000);
    append_xsdt_child(image, 0xb000, make_dmar_with_include_all_drhd(register_base, 0));

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::valid);
    REQUIRE(snapshot.dmar_units.size() == 1);
    CHECK(snapshot.dmar_units[0].register_base == register_base);
    CHECK(snapshot.dmar_units[0].register_length == 0x1000);
    CHECK(snapshot.dmar_units[0].segment == 0);
    CHECK(snapshot.dmar_units[0].include_all);
    CHECK(snapshot.dmar_host_address_width == 47);
}

TEST_CASE("AMD IVRS selects the highest supported IVHD without merging entries and keeps IVMD ownership",
          "[acpi_boot_tables][ivrs]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);
    image.additional_device_ranges.emplace_back(0xfed90000, 0x4000);
    append_xsdt_child(image, 0xb000, make_ivrs_compatibility_pair());

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    REQUIRE(snapshot.ivrs_state == iommu::amd_vi::ivrs_inventory_state::valid);
    REQUIRE(snapshot.amd_ivhd_records.size() == 2);
    REQUIRE(snapshot.amd_ivhd_device_entries.size() == 2);
    REQUIRE(snapshot.amd_ivmd_records.size() == 2);
    CHECK(snapshot.amd_ivhd_records[0].type == 0x10);
    CHECK_FALSE(snapshot.amd_ivhd_records[0].selected);
    CHECK(snapshot.amd_ivhd_records[1].type == 0x11);
    CHECK(snapshot.amd_ivhd_records[1].selected);
    CHECK(snapshot.amd_ivhd_records[0].device_entry_count == 1);
    CHECK(snapshot.amd_ivhd_records[1].device_entry_count == 1);
    CHECK(snapshot.amd_ivhd_device_entries[0].device_id == 0x0100);
    CHECK(snapshot.amd_ivhd_device_entries[1].device_id == 0x0101);
    CHECK(snapshot.amd_ivhd_records[0].ivmd_count == 1);
    CHECK(snapshot.amd_ivhd_records[1].ivmd_count == 1);
    CHECK(snapshot.amd_ivmd_records[0].ivhd_record_index == 0);
    CHECK(snapshot.amd_ivmd_records[1].ivhd_record_index == 1);
}

TEST_CASE("AMD IVRS rejects an unterminated DeviceID range without discarding ACPI", "[acpi_boot_tables][ivrs]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);
    image.additional_device_ranges.emplace_back(0xfed90000, 0x4000);
    append_xsdt_child(image, 0xb000, make_ivrs_unterminated_range());

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(snapshot.ivrs_state == iommu::amd_vi::ivrs_inventory_state::invalid);
    CHECK(snapshot.ivrs_error == arch::acpi_boot::error::malformed_subtable);
    CHECK(snapshot.amd_ivhd_records.empty());
    CHECK(snapshot.amd_ivhd_device_entries.empty());
}

TEST_CASE("AMD IVHD Type 40h parses EFR Register Image 2 when IVRS reports EFR support", "[acpi_boot_tables][ivrs]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);
    image.additional_device_ranges.emplace_back(0xfed90000, 0x4000);
    append_xsdt_child(image, 0xb000, make_ivrs_type40());

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    REQUIRE(snapshot.ivrs_state == iommu::amd_vi::ivrs_inventory_state::valid);
    REQUIRE(snapshot.amd_ivhd_records.size() == 1);
    CHECK(snapshot.amd_ivhd_records[0].type == 0x40);
    CHECK(snapshot.amd_ivhd_records[0].selected);
    CHECK(snapshot.amd_ivhd_records[0].extended_features_2 == 0x1122334455667788ull);
}

TEST_CASE("Invalid DMAR device ranges disable only the IOMMU inventory", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);
    append_xsdt_child(image, 0xb000, make_dmar_with_include_all_drhd(0xfed90000, 0));

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::invalid);
    CHECK(snapshot.dmar_units.empty());
}

TEST_CASE("DMAR retains validated DRHD and RMRR device scopes", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);

    constexpr u64 register_base = 0xfed90000;
    constexpr u64 reserved_base = 0x80000000;
    image.additional_device_ranges.emplace_back(register_base, 0x1000);
    image.additional_device_ranges.emplace_back(reserved_base, 0x1000);
    append_xsdt_child(image, 0xb000, make_dmar_with_drhd_and_rmrr(register_base, reserved_base));

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    REQUIRE(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::valid);
    REQUIRE(snapshot.dmar_units.size() == 1);
    REQUIRE(snapshot.dmar_reserved_regions.size() == 1);
    REQUIRE(snapshot.dmar_device_scopes.size() == 2);
    REQUIRE(snapshot.dmar_device_paths.size() == 2);
    CHECK(snapshot.dmar_reserved_regions[0].base == reserved_base);
    CHECK(snapshot.dmar_reserved_regions[0].length == 0x1000);
    CHECK(snapshot.dmar_reserved_regions[0].device_scope_count == 1);
    CHECK(snapshot.dmar_units[0].device_scope_count == 1);
    CHECK(snapshot.dmar_device_scopes[0].type == 1);
    CHECK(snapshot.dmar_device_paths[0].device == 3);
    CHECK(snapshot.dmar_device_paths[0].function == 1);
    CHECK(snapshot.dmar_device_paths[1].device == 3);
    CHECK(snapshot.dmar_device_paths[1].function == 1);
}

TEST_CASE("DMAR validates non-retained ATSR SATC and SIDP scopes", "[acpi_boot_tables][dmar]")
{
    constexpr u64 register_base = 0xfed90000;
    for (const u16 subtable_type : {u16(2), u16(5), u16(6)})
    {
        allocation_cleanup cleanup;
        memory::MemoryAllocator allocator;
        memory_allocator_scope allocator_scope(&allocator);
        firmware_image image;
        install_firmware(image);
        image.additional_device_ranges.emplace_back(register_base, 0x1000);
        append_xsdt_child(image, 0xb000, make_dmar_with_nonretained_scope_subtable(register_base, subtable_type));

        const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                      nullptr, &image};
        naos_test::iommu_firmware::snapshot snapshot(&allocator);
        arch::acpi_boot::error failure = arch::acpi_boot::error::none;

        INFO("DMAR subtable type " << subtable_type);
        REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
        INFO("DMAR parse error " << static_cast<u32>(snapshot.dmar_error));
        CHECK(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::valid);
        CHECK(snapshot.dmar_units.size() == 1);
        CHECK(failure == arch::acpi_boot::error::none);
    }
}

TEST_CASE("DMAR resolves an IOAPIC scope to its exact PCI requester ID", "[acpi_boot_tables][dmar][irq]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;

    iommu::intel_vtd::dmar_hardware_unit unit{};
    unit.segment = 2;
    unit.device_scope_start = 0;
    unit.device_scope_count = 1;
    snapshot.dmar_units.push_back(unit);
    snapshot.dmar_device_scopes.push_back(
        iommu::intel_vtd::dmar_device_scope{0, 1, iommu::intel_vtd::dmar_device_scope_type_ioapic, 0, 7, 0x20});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{11, 4});

    u16 requester_id = 0xffff;
    iommu::intel_vtd::source_resolution_error failure = iommu::intel_vtd::source_resolution_error::none;
    REQUIRE(iommu::intel_vtd::resolve_ioapic_requester_id(snapshot, unit, 7, nullptr, nullptr, requester_id, failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::none);
    CHECK(requester_id == ((0x20u << 8) | (11u << 3) | 4u));
}

TEST_CASE("DMAR resolves IOAPIC bridge paths by reading each upstream secondary bus", "[acpi_boot_tables][dmar][irq]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;

    iommu::intel_vtd::dmar_hardware_unit unit{};
    unit.segment = 4;
    unit.device_scope_start = 0;
    unit.device_scope_count = 1;
    snapshot.dmar_units.push_back(unit);
    snapshot.dmar_device_scopes.push_back(
        iommu::intel_vtd::dmar_device_scope{0, 2, iommu::intel_vtd::dmar_device_scope_type_ioapic, 0, 9, 0});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{2, 0});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{5, 3});

    dmar_path_reader reader{};
    reader.expected_segment = 4;
    reader.expected_bus = 0;
    reader.expected_device = 2;
    reader.expected_function = 0;
    reader.secondary_bus = 0x27;
    u16 requester_id = 0xffff;
    iommu::intel_vtd::source_resolution_error failure = iommu::intel_vtd::source_resolution_error::none;

    REQUIRE(iommu::intel_vtd::resolve_ioapic_requester_id(snapshot, unit, 9, read_bridge_info, &reader, requester_id,
                                                          failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::none);
    CHECK(reader.calls == 1);
    CHECK(requester_id == ((0x27u << 8) | (5u << 3) | 3u));
}

TEST_CASE("DMAR IOAPIC resolution rejects ambiguous scopes and failed bridge reads", "[acpi_boot_tables][dmar][irq]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;

    iommu::intel_vtd::dmar_hardware_unit unit{};
    unit.segment = 0;
    unit.device_scope_start = 0;
    unit.device_scope_count = 2;
    snapshot.dmar_units.push_back(unit);
    snapshot.dmar_device_scopes.push_back(
        iommu::intel_vtd::dmar_device_scope{0, 1, iommu::intel_vtd::dmar_device_scope_type_ioapic, 0, 2, 0});
    snapshot.dmar_device_scopes.push_back(
        iommu::intel_vtd::dmar_device_scope{1, 1, iommu::intel_vtd::dmar_device_scope_type_ioapic, 0, 2, 0});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{3, 0});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{4, 0});

    u16 requester_id = 0xffff;
    iommu::intel_vtd::source_resolution_error failure = iommu::intel_vtd::source_resolution_error::none;
    CHECK_FALSE(
        iommu::intel_vtd::resolve_ioapic_requester_id(snapshot, unit, 2, nullptr, nullptr, requester_id, failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::ambiguous_scope);
    CHECK(requester_id == 0);

    snapshot.dmar_units[0].device_scope_count = 1;
    snapshot.dmar_device_scopes[0].path_count = 2;
    dmar_path_reader reader{};
    reader.fail = true;
    failure = iommu::intel_vtd::source_resolution_error::none;
    requester_id = 0xffff;
    CHECK_FALSE(iommu::intel_vtd::resolve_ioapic_requester_id(snapshot, snapshot.dmar_units[0], 2, read_bridge_info,
                                                              &reader, requester_id, failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::config_read_failed);
    CHECK(requester_id == 0);

    reader.fail = false;
    reader.expected_device = 3;
    reader.primary_bus = 1;
    failure = iommu::intel_vtd::source_resolution_error::none;
    requester_id = 0xffff;
    CHECK_FALSE(iommu::intel_vtd::resolve_ioapic_requester_id(snapshot, snapshot.dmar_units[0], 2, read_bridge_info,
                                                              &reader, requester_id, failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::invalid_path);
    CHECK(requester_id == 0);
}

TEST_CASE("DMAR resolves PCI endpoint scopes before the INCLUDE_PCI_ALL fallback", "[acpi_boot_tables][dmar][pci]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;

    iommu::intel_vtd::dmar_hardware_unit endpoint_unit{};
    endpoint_unit.segment = 0;
    endpoint_unit.device_scope_count = 1;
    snapshot.dmar_units.push_back(endpoint_unit);
    iommu::intel_vtd::dmar_hardware_unit fallback_unit{};
    fallback_unit.segment = 0;
    fallback_unit.include_all = true;
    fallback_unit.flags = 1;
    snapshot.dmar_units.push_back(fallback_unit);
    snapshot.dmar_device_scopes.push_back(
        iommu::intel_vtd::dmar_device_scope{0, 1, iommu::intel_vtd::dmar_device_scope_type_pci_endpoint, 0, 0, 0});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{5, 0});

    u32 unit_index = 0xffffffff;
    u16 requester_id = 0xffff;
    iommu::intel_vtd::source_resolution_error failure = iommu::intel_vtd::source_resolution_error::none;
    REQUIRE(iommu::intel_vtd::resolve_pci_device_unit(snapshot, 0, 0, 5, 0, nullptr, nullptr, unit_index, requester_id,
                                                      failure));
    CHECK(unit_index == 0);
    CHECK(requester_id == (5u << 3));

    REQUIRE(iommu::intel_vtd::resolve_pci_device_unit(snapshot, 0, 0, 6, 0, nullptr, nullptr, unit_index, requester_id,
                                                      failure));
    CHECK(unit_index == 1);
    CHECK(requester_id == (6u << 3));
}

TEST_CASE("DMAR sub-hierarchy scopes include descendants using verified subordinate bus ranges",
          "[acpi_boot_tables][dmar][pci]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;

    iommu::intel_vtd::dmar_hardware_unit scoped_unit{};
    scoped_unit.segment = 3;
    scoped_unit.device_scope_count = 1;
    snapshot.dmar_units.push_back(scoped_unit);
    iommu::intel_vtd::dmar_hardware_unit fallback_unit{};
    fallback_unit.segment = 3;
    fallback_unit.include_all = true;
    fallback_unit.flags = 1;
    snapshot.dmar_units.push_back(fallback_unit);
    snapshot.dmar_device_scopes.push_back(
        iommu::intel_vtd::dmar_device_scope{0, 2, iommu::intel_vtd::dmar_device_scope_type_pci_subhierarchy, 0, 0, 0});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{2, 0});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{5, 3});

    dmar_scope_reader reader{};
    reader.bridges[0] = {3, 0, 2, 0, 1, 0, 0x20, 0x3f};
    reader.bridges[1] = {3, 0x20, 5, 3, 1, 0x20, 0x27, 0x2f};
    reader.bridge_count = 2;
    u32 unit_index = 0xffffffff;
    u16 requester_id = 0xffff;
    iommu::intel_vtd::source_resolution_error failure = iommu::intel_vtd::source_resolution_error::none;

    REQUIRE(iommu::intel_vtd::resolve_pci_device_unit(snapshot, 3, 0x29, 7, 1, read_scope_bridge_info, &reader,
                                                      unit_index, requester_id, failure));
    CHECK(unit_index == 0);
    CHECK(requester_id == ((0x29u << 8) | (7u << 3) | 1u));
    CHECK(reader.calls == 2);

    reader.calls = 0;
    REQUIRE(iommu::intel_vtd::resolve_pci_device_unit(snapshot, 3, 0x30, 7, 1, read_scope_bridge_info, &reader,
                                                      unit_index, requester_id, failure));
    CHECK(unit_index == 1);
    CHECK(requester_id == ((0x30u << 8) | (7u << 3) | 1u));
}

TEST_CASE("DMAR PCI resolution rejects ambiguous explicit units and uncertain bridge paths",
          "[acpi_boot_tables][dmar][pci]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    for (u32 index = 0; index < 2; ++index)
    {
        iommu::intel_vtd::dmar_hardware_unit unit{};
        unit.segment = 0;
        unit.device_scope_start = index;
        unit.device_scope_count = 1;
        snapshot.dmar_units.push_back(unit);
        snapshot.dmar_device_scopes.push_back(iommu::intel_vtd::dmar_device_scope{
            index, 1, iommu::intel_vtd::dmar_device_scope_type_pci_endpoint, 0, 0, 0});
        snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{5, 0});
    }

    u32 unit_index = 0xffffffff;
    u16 requester_id = 0xffff;
    iommu::intel_vtd::source_resolution_error failure = iommu::intel_vtd::source_resolution_error::none;
    CHECK_FALSE(iommu::intel_vtd::resolve_pci_device_unit(snapshot, 0, 0, 5, 0, nullptr, nullptr, unit_index,
                                                          requester_id, failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::ambiguous_unit);
    CHECK(unit_index == 0xffffffff);
    CHECK(requester_id == 0);

    snapshot.dmar_units.clear();
    snapshot.dmar_device_scopes.clear();
    snapshot.dmar_device_paths.clear();
    iommu::intel_vtd::dmar_hardware_unit scoped_unit{};
    scoped_unit.segment = 0;
    scoped_unit.device_scope_count = 1;
    snapshot.dmar_units.push_back(scoped_unit);
    iommu::intel_vtd::dmar_hardware_unit fallback_unit{};
    fallback_unit.segment = 0;
    fallback_unit.include_all = true;
    fallback_unit.flags = 1;
    snapshot.dmar_units.push_back(fallback_unit);
    snapshot.dmar_device_scopes.push_back(
        iommu::intel_vtd::dmar_device_scope{0, 1, iommu::intel_vtd::dmar_device_scope_type_pci_subhierarchy, 0, 0, 0});
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{2, 0});

    dmar_scope_reader reader{};
    reader.fail = true;
    unit_index = 0xffffffff;
    requester_id = 0xffff;
    failure = iommu::intel_vtd::source_resolution_error::none;
    CHECK_FALSE(iommu::intel_vtd::resolve_pci_device_unit(snapshot, 0, 0x20, 7, 1, read_scope_bridge_info, &reader,
                                                          unit_index, requester_id, failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::config_read_failed);
    CHECK(unit_index == 0xffffffff);
    CHECK(requester_id == 0);

    snapshot.dmar_device_scopes[0].path_count = 2;
    snapshot.dmar_device_paths.push_back(iommu::intel_vtd::dmar_device_path{5, 3});
    reader.fail = false;
    reader.bridge_count = 2;
    reader.bridges[0] = {0, 0, 2, 0, 1, 0, 0x20, 0x20};
    reader.bridges[1] = {0, 0x20, 5, 3, 1, 0x20, 0x27, 0x2f};
    unit_index = 0xffffffff;
    requester_id = 0xffff;
    failure = iommu::intel_vtd::source_resolution_error::none;
    CHECK_FALSE(iommu::intel_vtd::resolve_pci_device_unit(snapshot, 0, 0x29, 7, 1, read_scope_bridge_info, &reader,
                                                          unit_index, requester_id, failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::invalid_path);
    CHECK(unit_index == 0xffffffff);
    CHECK(requester_id == 0);
}

TEST_CASE("DMAR PCI resolution rejects INCLUDE_PCI_ALL before later segment units", "[acpi_boot_tables][dmar][pci]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    snapshot.dmar_state = iommu::intel_vtd::dmar_inventory_state::valid;
    iommu::intel_vtd::dmar_hardware_unit fallback_unit{};
    fallback_unit.segment = 0;
    fallback_unit.include_all = true;
    fallback_unit.flags = 1;
    snapshot.dmar_units.push_back(fallback_unit);
    iommu::intel_vtd::dmar_hardware_unit later_unit{};
    later_unit.segment = 0;
    snapshot.dmar_units.push_back(later_unit);

    u32 unit_index = 0xffffffff;
    u16 requester_id = 0xffff;
    iommu::intel_vtd::source_resolution_error failure = iommu::intel_vtd::source_resolution_error::none;
    CHECK_FALSE(iommu::intel_vtd::resolve_pci_device_unit(snapshot, 0, 0, 1, 0, nullptr, nullptr, unit_index,
                                                          requester_id, failure));
    CHECK(failure == iommu::intel_vtd::source_resolution_error::invalid_inventory_order);
}

TEST_CASE("Malformed DMAR scopes clear partial inventory without rejecting ACPI", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);

    constexpr u64 register_base = 0xfed90000;
    image.additional_device_ranges.emplace_back(register_base, 0x1000);
    append_xsdt_child(image, 0xb000, make_dmar_with_include_all_drhd_and_scope(register_base, 6));

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::invalid);
    CHECK(snapshot.dmar_error == arch::acpi_boot::error::malformed_subtable);
    CHECK(snapshot.dmar_units.empty());
    CHECK(snapshot.dmar_reserved_regions.empty());
    CHECK(snapshot.dmar_device_scopes.empty());
    CHECK(snapshot.dmar_device_paths.empty());
    CHECK(failure == arch::acpi_boot::error::none);
}

TEST_CASE("DMAR rejects PCI endpoint scopes on INCLUDE_PCI_ALL units", "[acpi_boot_tables][dmar][pci]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);

    constexpr u64 register_base = 0xfed90000;
    image.additional_device_ranges.emplace_back(register_base, 0x1000);
    append_xsdt_child(image, 0xb000,
                      make_dmar_with_include_all_drhd_and_scope(register_base,
                                                                iommu::intel_vtd::dmar_device_scope_type_pci_endpoint));

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::invalid);
    CHECK(snapshot.dmar_error == arch::acpi_boot::error::malformed_subtable);
    CHECK(snapshot.dmar_units.empty());
    CHECK(snapshot.dmar_device_scopes.empty());
    CHECK(snapshot.dmar_device_paths.empty());
}

TEST_CASE("DMAR requires INCLUDE_PCI_ALL after scoped units of the same segment", "[acpi_boot_tables][dmar][pci]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);

    constexpr u64 register_base = 0xfed90000;
    image.additional_device_ranges.emplace_back(register_base, 0x1000);
    image.additional_device_ranges.emplace_back(register_base + 0x1000, 0x1000);
    append_xsdt_child(image, 0xb000, make_dmar_with_include_all_before_scoped_drhd(register_base));

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::invalid);
    CHECK(snapshot.dmar_error == arch::acpi_boot::error::malformed_subtable);
    CHECK(snapshot.dmar_units.empty());
    CHECK(snapshot.dmar_device_scopes.empty());
    CHECK(snapshot.dmar_device_paths.empty());
}

TEST_CASE("An unadmitted DMAR reserved-memory range invalidates the full IOMMU inventory", "[acpi_boot_tables]")
{
    allocation_cleanup cleanup;
    memory::MemoryAllocator allocator;
    memory_allocator_scope allocator_scope(&allocator);
    firmware_image image;
    install_firmware(image);

    constexpr u64 register_base = 0xfed90000;
    constexpr u64 reserved_base = 0x80000000;
    image.additional_device_ranges.emplace_back(register_base, 0x1000);
    append_xsdt_child(image, 0xb000, make_dmar_with_drhd_and_rmrr(register_base, reserved_base));

    const arch::acpi_boot::physical_source source{map_firmware, firmware_range_valid, firmware_device_range_valid,
                                                  nullptr, &image};
    naos_test::iommu_firmware::snapshot snapshot(&allocator);
    arch::acpi_boot::error failure = arch::acpi_boot::error::none;

    REQUIRE(naos_test::iommu_firmware::build_snapshot(source, 0x1000, 17, 1, snapshot, failure));
    CHECK(snapshot.dmar_state == iommu::intel_vtd::dmar_inventory_state::invalid);
    CHECK(snapshot.dmar_error == arch::acpi_boot::error::invalid_address);
    CHECK(snapshot.dmar_units.empty());
    CHECK(snapshot.dmar_reserved_regions.empty());
    CHECK(snapshot.dmar_device_scopes.empty());
    CHECK(snapshot.dmar_device_paths.empty());
}

TEST_CASE("ACPI SystemMemory excludes boot-owned physical ranges", "[acpi_boot_tables]")
{
    kernel_start_args args{};
    args.kernel_base = 0x100000;
    args.kernel_size = 0x200000;
    args.data_base = 0x400000;
    args.data_size = 0x1000;
    args.boot_module_count = 1;
    args.boot_modules[0].start = 0x500000;
    args.boot_modules[0].size = 0x2000;

    CHECK(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x101000, 0x1000));
    CHECK(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x0fffff, 2));
    CHECK(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x400800, 0x100));
    CHECK(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x501000, 0x2000));
    CHECK_FALSE(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x0fffff, 1));
    CHECK_FALSE(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x300000, 0x1000));
    CHECK_FALSE(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x600000, 0x1000));
}

TEST_CASE("ACPI SystemMemory boot-owned range checks fail closed on malformed ranges", "[acpi_boot_tables]")
{
    kernel_start_args args{};
    args.kernel_base = ~u64(0) - 3;
    args.kernel_size = 8;
    CHECK(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x1000, 1));

    args = {};
    args.boot_module_count = max_boot_modules + 1;
    CHECK(arch::acpi_boot::overlaps_boot_owned_memory(args, 0x1000, 1));

    args = {};
    CHECK(arch::acpi_boot::overlaps_boot_owned_memory(args, ~u64(0) - 1, 4));
}

TEST_CASE("ACPI SystemMemory admits only contained firmware-reserved ranges", "[acpi_boot_tables]")
{
    kernel_start_args args{};
    args.kernel_base = 0x100000;
    args.kernel_size = 0x1000;
    args.data_base = 0x200000;
    args.data_size = 0x1000;
    args.boot_module_count = 1;
    args.boot_modules[0].start = 0x300000;
    args.boot_modules[0].size = 0x1000;

    const std::array<kernel_memory_map_item, 7> map{{
        {0x100000, 0x400000, map_type_t::reserved},
        {0x700000, 0x2000, map_type_t::reserved},
        {0x780000, 0x2000, map_type_t::acpi},
        {0x800000, 0x2000, map_type_t::nvs},
        {0x900000, 0x2000, map_type_t::available},
        {0xa00000, 0x2000, map_type_t::badram},
        {0xb00000, 0x2000, static_cast<map_type_t>(6)},
    }};

    CHECK(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0x700100, 0x100));
    CHECK(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0x780100, 0x100));
    CHECK(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0x800100, 0x100));
    CHECK_FALSE(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0x100000, 0x100));
    CHECK_FALSE(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0x200800, 0x100));
    CHECK_FALSE(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0x300800, 0x100));
    CHECK_FALSE(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0x900100, 0x100));
    CHECK_FALSE(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0xa00100, 0x100));
    CHECK_FALSE(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0xb00100, 0x100));
    CHECK_FALSE(arch::acpi_boot::is_device_range_valid(args, map.data(), map.size(), 0x801000, 0x2000));

    const std::array<kernel_memory_map_item, 2> conflicting_map{{
        {0x800000, 0x2000, map_type_t::nvs},
        {0x800800, 0x100, map_type_t::available},
    }};
    CHECK_FALSE(
        arch::acpi_boot::is_device_range_valid(args, conflicting_map.data(), conflicting_map.size(), 0x800800, 0x80));
}

TEST_CASE("IOMMU register apertures admit unmapped MMIO holes but reject RAM and boot-owned memory",
          "[acpi_boot_tables]")
{
    kernel_start_args args{};
    args.kernel_base = 0x100000;
    args.kernel_size = 0x100000;
    args.data_base = 0x300000;
    args.data_size = 0x1000;
    args.boot_module_count = 1;
    args.boot_modules[0].start = 0x500000;
    args.boot_modules[0].size = 0x2000;

    const std::array<kernel_memory_map_item, 3> map{{
        {0x100000, 0x800000, map_type_t::available},
        {0xfed1c000, 0x4000, map_type_t::reserved},
        {0xfeffc000, 0x4000, map_type_t::reserved},
    }};

    CHECK(arch::acpi_boot::is_iommu_register_range_valid(args, map.data(), map.size(), 0xfed90000, 0x1000));
    CHECK(arch::acpi_boot::is_iommu_register_range_valid(args, map.data(), map.size(), 0xfed1c000, 0x1000));
    CHECK_FALSE(arch::acpi_boot::is_iommu_register_range_valid(args, map.data(), map.size(), 0x700000, 0x1000));
    CHECK_FALSE(arch::acpi_boot::is_iommu_register_range_valid(args, map.data(), map.size(), 0x300000, 0x1000));
    CHECK_FALSE(arch::acpi_boot::is_iommu_register_range_valid(args, map.data(), map.size(), 0x500000, 0x1000));
    CHECK_FALSE(arch::acpi_boot::is_iommu_register_range_valid(args, map.data(), map.size(), ~u64(0) - 0xfff, 0x2000));

    const std::array<kernel_memory_map_item, 2> conflicting_map{{
        {0xfed90000, 0x1000, map_type_t::reserved},
        {0xfed90800, 0x100, map_type_t::available},
    }};
    CHECK_FALSE(arch::acpi_boot::is_iommu_register_range_valid(args, conflicting_map.data(), conflicting_map.size(),
                                                               0xfed90000, 0x1000));
}
