#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/dma_quiesce.hpp"

#include <array>

namespace
{
struct fake_function
{
    u8 bus = 0;
    u8 device = 0;
    u8 function = 0;
    u16 vendor = 0xffff;
    u8 header_type = 0;
    u16 command = 0;
    bool present = false;
    bool ignore_command_write = false;
    u32 command_writes = 0;
};

struct fake_ecam
{
    std::array<fake_function, 8> functions{};
    u32 read_count = 0;
    bool fail_read = false;
};

fake_function *find_function(fake_ecam &ecam, u8 bus, u8 device, u8 function)
{
    for (auto &entry : ecam.functions)
    {
        if (entry.bus == bus && entry.device == device && entry.function == function && entry.present)
            return &entry;
    }
    return nullptr;
}

bool read_identity(void *context, u8 bus, u8 device, u8 function, u16 &vendor, u8 &header_type)
{
    auto &ecam = *static_cast<fake_ecam *>(context);
    ecam.read_count++;
    if (ecam.fail_read)
        return false;
    const auto *entry = find_function(ecam, bus, device, function);
    vendor = entry == nullptr ? 0xffff : entry->vendor;
    header_type = entry == nullptr ? 0 : entry->header_type;
    return true;
}

bool read_command(void *context, u8 bus, u8 device, u8 function, u16 &command)
{
    auto &ecam = *static_cast<fake_ecam *>(context);
    auto *entry = find_function(ecam, bus, device, function);
    if (entry == nullptr)
        return false;
    command = entry->command;
    return true;
}

bool write_command(void *context, u8 bus, u8 device, u8 function, u16 command)
{
    auto &ecam = *static_cast<fake_ecam *>(context);
    auto *entry = find_function(ecam, bus, device, function);
    if (entry == nullptr)
        return false;
    entry->command_writes++;
    if (!entry->ignore_command_write)
        entry->command = command;
    return true;
}

pci::dma_quiesce::config_ops operations(fake_ecam &ecam) { return {read_identity, read_command, write_command, &ecam}; }

} // namespace

TEST_CASE("PCI DMA quiesce clears only bus-master enable on every present function", "[pci][dma]")
{
    fake_ecam ecam{};
    ecam.functions[0] = {32, 0, 0, 0x8086, 0x80, 0x0007, true};
    ecam.functions[1] = {32, 0, 1, 0x1234, 0x00, 0x0006, true};
    ecam.functions[2] = {32, 0, 2, 0xffff, 0x00, 0x0007, false};
    ecam.functions[3] = {32, 1, 0, 0x1af4, 0x00, 0x0003, true};

    const pci::ecam_region region{0xe0000000, 0, 32, 32, 1};
    auto io = operations(ecam);
    pci::dma_quiesce::result result{};

    REQUIRE(pci::dma_quiesce::quiesce_region(region, io, result) == pci::dma_quiesce::error::none);
    CHECK(result.functions_seen == 3);
    CHECK(result.bus_masters_disabled == 2);
    CHECK(ecam.functions[0].command == 0x0003);
    CHECK(ecam.functions[1].command == 0x0002);
    CHECK(ecam.functions[3].command == 0x0003);
    CHECK(ecam.functions[0].command_writes == 1);
    CHECK(ecam.functions[1].command_writes == 1);
    CHECK(ecam.functions[3].command_writes == 0);
}

TEST_CASE("PCI DMA quiesce fails closed if bus mastering remains enabled", "[pci][dma]")
{
    fake_ecam ecam{};
    ecam.functions[0] = {0, 0, 0, 0x8086, 0x00, 0x0007, true, true};
    const pci::ecam_region region{0xe0000000, 0, 0, 0, 1};
    auto io = operations(ecam);
    pci::dma_quiesce::result result{};

    CHECK(pci::dma_quiesce::quiesce_region(region, io, result) == pci::dma_quiesce::error::bus_master_still_enabled);
    CHECK(ecam.functions[0].command_writes == 1);
    CHECK(result.functions_seen == 1);
}

TEST_CASE("PCI bus-master disable remains available while unauthorised enable fails closed", "[pci][dma]")
{
    fake_ecam ecam{};
    ecam.functions[0] = {0, 3, 2, 0x1234, 0x00, 0x0003, true};
    auto io = operations(ecam);

    CHECK(pci::dma_quiesce::set_bus_master(io, 0, 3, 2, true) == pci::dma_quiesce::error::domain_required);
    CHECK(ecam.functions[0].command == 0x0003);
    CHECK(ecam.functions[0].command_writes == 0);

    CHECK(pci::dma_quiesce::set_bus_master(io, 0, 3, 2, false) == pci::dma_quiesce::error::none);
    CHECK(ecam.functions[0].command == 0x0003);
    CHECK(ecam.functions[0].command_writes == 0);

    ecam.functions[0].command = 0x0007;
    ecam.functions[0].ignore_command_write = true;
    CHECK(pci::dma_quiesce::set_bus_master(io, 0, 3, 2, false) == pci::dma_quiesce::error::bus_master_still_enabled);
    CHECK(ecam.functions[0].command == 0x0007);
    CHECK(ecam.functions[0].command_writes == 1);
}

TEST_CASE("PCI DMA quiesce rejects malformed ranges and config read failures", "[pci][dma]")
{
    fake_ecam ecam{};
    auto io = operations(ecam);
    pci::dma_quiesce::result result{};
    const pci::ecam_region malformed{0xe0000000, 0, 9, 8, 1};
    CHECK(pci::dma_quiesce::quiesce_region(malformed, io, result) == pci::dma_quiesce::error::invalid_region);
    CHECK(ecam.read_count == 0);

    ecam.fail_read = true;
    const pci::ecam_region valid{0xe0000000, 0, 0, 0, 1};
    CHECK(pci::dma_quiesce::quiesce_region(valid, io, result) == pci::dma_quiesce::error::config_read_failed);
}
