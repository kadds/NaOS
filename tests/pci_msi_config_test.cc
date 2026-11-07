#include <array>

#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/interrupt_event.hpp"
#include "kernel/pci/msi_capability.hpp"
#include "kernel/pci/msi_config.hpp"
#include "kernel/pci/msi_message.hpp"
#include "kernel/pci/msi_route_lifecycle.hpp"
#include "kernel/pci/msi_route_registry.hpp"

namespace
{
struct fake_config
{
    std::array<u32, 64> words{};
    std::array<u32, 16> msix_table{};
    u32 writes = 0;
    u32 msix_table_writes = 0;
    u32 fail_write = 0;
    u32 fail_msix_table_write = 0;
    u32 msix_capability_header_reads = 0;
    u32 enable_msix_on_header_read = 0;
    u16 msix_capability_offset = 0;
    bool fail_reads = false;
};

bool read_config(void *context, u16 offset, u32 &value)
{
    auto &config = *static_cast<fake_config *>(context);
    if (config.fail_reads || (offset & 3) != 0 || offset >= 0x100)
        return false;
    if (offset == config.msix_capability_offset && offset != 0)
    {
        config.msix_capability_header_reads++;
        if (config.enable_msix_on_header_read != 0 &&
            config.msix_capability_header_reads == config.enable_msix_on_header_read)
            config.words[offset / 4] |= 1u << 31;
    }
    value = config.words[offset / 4];
    return true;
}

bool write_config(void *context, u16 offset, u32 value)
{
    auto &config = *static_cast<fake_config *>(context);
    config.writes++;
    if ((offset & 3) != 0 || offset >= 0x100 || config.writes == config.fail_write)
        return false;
    config.words[offset / 4] = value;
    return true;
}

bool read_msix_table(void *context, u64 offset, u32 &value)
{
    auto &config = *static_cast<fake_config *>(context);
    if ((offset & 3) != 0 || offset >= config.msix_table.size() * sizeof(u32))
        return false;
    value = config.msix_table[offset / sizeof(u32)];
    return true;
}

bool write_msix_table(void *context, u64 offset, u32 value)
{
    auto &config = *static_cast<fake_config *>(context);
    config.msix_table_writes++;
    if ((offset & 3) != 0 || offset >= config.msix_table.size() * sizeof(u32) ||
        config.msix_table_writes == config.fail_msix_table_write)
        return false;
    config.msix_table[offset / sizeof(u32)] = value;
    return true;
}

void set_msi_capability(fake_config &config, u8 offset, bool address_64_bit, bool maskable, u8 maximum_messages)
{
    const u8 message_count_encoding = maximum_messages == 1 ? 0 : maximum_messages == 2 ? 1 : 2;
    u16 control = static_cast<u16>(message_count_encoding << 1);
    if (address_64_bit)
        control |= 1u << 7;
    if (maskable)
        control |= 1u << 8;
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = offset;
    config.words[offset / 4] = 0x05u | (static_cast<u32>(control) << 16);
}

void set_msix_capability(fake_config &config, u8 offset, u16 control, u32 table, u32 pba)
{
    config.msix_capability_offset = offset;
    config.words[0x04 / 4] = 1u << 20;
    config.words[0x34 / 4] = offset;
    config.words[offset / 4] = 0x11u | (static_cast<u32>(control) << 16);
    config.words[(offset + 4) / 4] = table;
    config.words[(offset + 8) / 4] = pba;
}

bool find_msi(fake_config &config, pci::msi_capability &capability)
{
    pci::msi_capability_error failure = pci::msi_capability_error::none;
    return pci::find_msi_capability(read_config, &config, capability, failure) &&
           failure == pci::msi_capability_error::none;
}

enum class teardown_operation : u8
{
    disable,
    drain,
    retire_irte,
    restore,
};

struct fake_teardown
{
    std::array<teardown_operation, 5> operations{};
    u32 operation_count = 0;
    u32 fail_at = ~u32{0};
};

bool record_teardown_operation(void *context, teardown_operation operation)
{
    auto &fake = *static_cast<fake_teardown *>(context);
    const u32 position = fake.operation_count++;
    fake.operations[position] = operation;
    return position != fake.fail_at;
}

bool disable_route(void *context) { return record_teardown_operation(context, teardown_operation::disable); }
bool drain_route(void *context) { return record_teardown_operation(context, teardown_operation::drain); }
bool retire_route_irte(void *context) { return record_teardown_operation(context, teardown_operation::retire_irte); }
bool restore_route(void *context) { return record_teardown_operation(context, teardown_operation::restore); }
} // namespace

TEST_CASE("PCI MSI route preparation disables, masks and programs a 64-bit MSI", "[pci][msi]")
{
    fake_config config{};
    set_msi_capability(config, 0x50, true, true, 4);
    config.words[(0x50 + 4) / 4] = 0xfee01000;
    config.words[(0x50 + 8) / 4] = 0x12345678;
    config.words[(0x50 + 12) / 4] = 0x00000045;
    config.words[(0x50 + 16) / 4] = 0x0000000e;

    pci::msi_capability capability{};
    REQUIRE(find_msi(config, capability));
    const pci::msi_config_ops operations{read_config, write_config, &config};
    const pci::msi_message message{0xfee00000ull | (0x35ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msi_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;

    REQUIRE(pci::prepare_remappable_msi(operations, capability, message, previous, failure));
    CHECK(failure == pci::msi_config_error::none);
    CHECK(previous.capability.offset == 0x50);
    CHECK((config.words[0x50 / 4] & (1u << 16)) == 0);
    CHECK((config.words[0x50 / 4] & (7u << 20)) == 0);
    CHECK(config.words[(0x50 + 4) / 4] == static_cast<u32>(message.address));
    CHECK(config.words[(0x50 + 8) / 4] == static_cast<u32>(message.address >> 32));
    CHECK(config.words[(0x50 + 12) / 4] == 0);
    CHECK(config.words[(0x50 + 16) / 4] == 0x0f);
    CHECK((config.words[0x50 / 4] & 0x0000ffffu) == 0x00000005u);

    REQUIRE(pci::set_remappable_msi_enabled(operations, previous, true, failure));
    CHECK((config.words[(0x50 + 16) / 4] & 1u) == 0);
    CHECK((config.words[0x50 / 4] & (1u << 16)) != 0);

    config.words[(0x50 + 16) / 4] |= 1u;
    REQUIRE(pci::set_remappable_msi_enabled(operations, previous, true, failure));
    CHECK((config.words[(0x50 + 16) / 4] & 1u) == 0);

    REQUIRE(pci::set_remappable_msi_enabled(operations, previous, false, failure));
    CHECK((config.words[(0x50 + 16) / 4] & 1u) != 0);
    CHECK((config.words[0x50 / 4] & (1u << 16)) == 0);
}

TEST_CASE("PCI MSI route preparation handles 32-bit unmaskable MSI", "[pci][msi]")
{
    fake_config config{};
    set_msi_capability(config, 0x40, false, false, 1);
    config.words[(0x40 + 4) / 4] = 0xfee00000;
    config.words[(0x40 + 8) / 4] = 0x00000040;

    pci::msi_capability capability{};
    REQUIRE(find_msi(config, capability));
    const pci::msi_config_ops operations{read_config, write_config, &config};
    const pci::msi_message message{0xfee00000ull | (7ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msi_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;

    REQUIRE(pci::prepare_remappable_msi(operations, capability, message, previous, failure));
    CHECK(config.words[(0x40 + 4) / 4] == static_cast<u32>(message.address));
    CHECK(config.words[(0x40 + 8) / 4] == 0);
    CHECK_FALSE(capability.per_vector_masking);
}

TEST_CASE("PCI MSI route preparation rejects active firmware routes before writing", "[pci][msi]")
{
    fake_config config{};
    set_msi_capability(config, 0x40, false, false, 1);
    config.words[0x40 / 4] |= 1u << 16;
    pci::msi_capability capability{};
    REQUIRE(find_msi(config, capability));
    const pci::msi_config_ops operations{read_config, write_config, &config};
    const pci::msi_message message{0xfee00000ull | (1ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msi_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;

    CHECK_FALSE(pci::prepare_remappable_msi(operations, capability, message, previous, failure));
    CHECK(failure == pci::msi_config_error::msi_already_enabled);
    CHECK(config.writes == 0);
    CHECK((config.words[0x40 / 4] & (1u << 16)) != 0);
}

TEST_CASE("PCI MSI route refuses to coexist with an enabled MSI-X capability", "[pci][msi][msix]")
{
    fake_config config{};
    set_msi_capability(config, 0x40, false, false, 1);
    config.words[0x40 / 4] |= 0x50u << 8;
    config.words[0x50 / 4] = 0x11u | (1u << 31); // MSI-X Enable is control bit 15.
    config.words[0x54 / 4] = 0;
    config.words[0x58 / 4] = 8;

    pci::msi_capability capability{};
    REQUIRE(find_msi(config, capability));
    const pci::msi_config_ops operations{read_config, write_config, &config};
    const pci::msi_message message{0xfee00000ull | (1ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msi_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;

    CHECK_FALSE(pci::prepare_remappable_msi(operations, capability, message, previous, failure));
    CHECK(failure == pci::msi_config_error::msix_already_enabled);
    CHECK(config.writes == 0);
    CHECK((config.words[0x40 / 4] & (1u << 16)) == 0);
}

TEST_CASE("PCI MSI-X route masks every vector before setup and restores after disable", "[pci][msix]")
{
    fake_config config{};
    set_msix_capability(config, 0x40, 3, 0x00002002, 0x00001001); // Four entries, table BIR 2.
    config.msix_table[0] = 0x11110000;
    config.msix_table[1] = 0x22220000;
    config.msix_table[2] = 0x00000045;
    config.msix_table[3] = 0;
    config.msix_table[7] = 1;
    config.msix_table[11] = 0;
    config.msix_table[15] = 1;
    const auto original_table = config.msix_table;

    const pci::msix_config_ops operations{read_config,      write_config, read_msix_table,
                                          write_msix_table, &config,      sizeof(config.msix_table)};
    const pci::msi_message message{0xfee00000ull | (0x35ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msix_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;

    REQUIRE(pci::prepare_remappable_msix(operations, message, previous, failure));
    CHECK(failure == pci::msi_config_error::none);
    CHECK((config.words[0x40 / 4] & (1u << 31)) == 0);
    CHECK((config.words[0x40 / 4] & (1u << 30)) != 0);
    CHECK(config.msix_table[0] == static_cast<u32>(message.address));
    CHECK(config.msix_table[1] == static_cast<u32>(message.address >> 32));
    CHECK(config.msix_table[2] == message.data);
    for (u32 entry = 0; entry < 4; ++entry)
        CHECK((config.msix_table[entry * 4 + 3] & 1u) != 0);

    REQUIRE(pci::set_remappable_msix_enabled(operations, previous, true, failure));
    CHECK((config.words[0x40 / 4] & (1u << 31)) != 0);
    CHECK((config.words[0x40 / 4] & (1u << 30)) == 0);
    CHECK((config.msix_table[3] & 1u) == 0);
    CHECK((config.msix_table[7] & 1u) != 0);
    CHECK((config.msix_table[11] & 1u) != 0);
    CHECK((config.msix_table[15] & 1u) != 0);

    REQUIRE(pci::set_remappable_msix_enabled(operations, previous, false, failure));
    CHECK((config.words[0x40 / 4] & (1u << 31)) == 0);
    CHECK((config.words[0x40 / 4] & (1u << 30)) != 0);
    REQUIRE(pci::restore_msix_configuration(operations, previous, failure));
    CHECK(failure == pci::msi_config_error::none);
    CHECK(config.msix_table == original_table);
    CHECK((config.words[0x40 / 4] & (1u << 31)) == 0);
    CHECK((config.words[0x40 / 4] & (1u << 30)) == 0);
}

TEST_CASE("PCI MSI-X table write uncertainty does not unmask the function", "[pci][msix]")
{
    fake_config config{};
    set_msix_capability(config, 0x40, 0, 0, 8);
    const pci::msix_config_ops operations{read_config,      write_config, read_msix_table,
                                          write_msix_table, &config,      sizeof(config.msix_table)};
    const pci::msi_message message{0xfee00000ull | (0x35ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msix_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;
    config.fail_msix_table_write = 1;

    CHECK_FALSE(pci::prepare_remappable_msix(operations, message, previous, failure));
    CHECK(failure == pci::msi_config_error::hardware_outcome_unknown);
    CHECK((config.words[0x40 / 4] & (1u << 30)) != 0);
    CHECK((config.words[0x40 / 4] & (1u << 31)) == 0);
}

TEST_CASE("PCI MSI-X route refuses to retry a partially completed function-mask transition", "[pci][msix]")
{
    fake_config config{};
    set_msix_capability(config, 0x40, 0, 0, 8);
    const pci::msix_config_ops operations{read_config,      write_config, read_msix_table,
                                          write_msix_table, &config,      sizeof(config.msix_table)};
    const pci::msi_message message{0xfee00000ull | (0x35ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msix_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;
    REQUIRE(pci::prepare_remappable_msix(operations, message, previous, failure));

    // Simulate a write that reached hardware, but whose readback outcome was
    // lost: MSI-X is enabled while Function Mask still blocks all vectors.
    config.words[0x40 / 4] |= 1u << 31;
    const u32 config_writes = config.writes;
    const u32 table_writes = config.msix_table_writes;
    CHECK_FALSE(pci::set_remappable_msix_enabled(operations, previous, true, failure));
    CHECK(failure == pci::msi_config_error::hardware_outcome_unknown);
    CHECK_FALSE(pci::set_remappable_msix_enabled(operations, previous, false, failure));
    CHECK(failure == pci::msi_config_error::hardware_outcome_unknown);
    CHECK(config.writes == config_writes);
    CHECK(config.msix_table_writes == table_writes);
}

TEST_CASE("PCI MSI-X setup rechecks Enable immediately before changing the table", "[pci][msix]")
{
    fake_config config{};
    set_msix_capability(config, 0x40, 0, 0, 8);
    const pci::msix_config_ops operations{read_config,      write_config, read_msix_table,
                                          write_msix_table, &config,      sizeof(config.msix_table)};
    const pci::msi_message message{0xfee00000ull | (0x35ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msix_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;
    config.enable_msix_on_header_read = 3;

    CHECK_FALSE(pci::prepare_remappable_msix(operations, message, previous, failure));
    CHECK(failure == pci::msi_config_error::msix_already_enabled);
    CHECK(config.writes == 0);
    CHECK(config.msix_table_writes == 0);
}

TEST_CASE("PCI MSI config write failure reports unknown hardware outcome", "[pci][msi]")
{
    fake_config config{};
    set_msi_capability(config, 0x40, false, false, 1);
    pci::msi_capability capability{};
    REQUIRE(find_msi(config, capability));
    const pci::msi_config_ops operations{read_config, write_config, &config};
    const pci::msi_message message{0xfee00000ull | (1ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msi_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;
    config.fail_write = 1;

    CHECK_FALSE(pci::prepare_remappable_msi(operations, capability, message, previous, failure));
    CHECK(failure == pci::msi_config_error::hardware_outcome_unknown);
}

TEST_CASE("PCI MSI preparation rejects values outside the capability field widths", "[pci][msi]")
{
    fake_config config{};
    set_msi_capability(config, 0x40, false, false, 1);
    pci::msi_capability capability{};
    REQUIRE(find_msi(config, capability));
    const pci::msi_config_ops operations{read_config, write_config, &config};
    pci::msi_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;

    const std::array<pci::msi_message, 2> invalid_messages{{{0xfee00000ull, 0x10000}, {0x100000000ull, 0}}};
    for (const auto &message : invalid_messages)
    {
        CHECK_FALSE(pci::prepare_remappable_msi(operations, capability, message, previous, failure));
        CHECK(failure == pci::msi_config_error::invalid_message);
    }
    CHECK(config.writes == 0);
}

TEST_CASE("PCI MSI configuration restore checks the live mask and restores the original state", "[pci][msi]")
{
    fake_config config{};
    set_msi_capability(config, 0x50, true, true, 4);
    config.words[0x50 / 4] |= 1u << 20;
    config.words[(0x50 + 4) / 4] = 0xfee01000;
    config.words[(0x50 + 8) / 4] = 0x12345678;
    config.words[(0x50 + 12) / 4] = 0x00000045;
    config.words[(0x50 + 16) / 4] = 0x0000000e;
    const u32 original_header = config.words[0x50 / 4];
    const u32 original_address_low = config.words[(0x50 + 4) / 4];
    const u32 original_address_high = config.words[(0x50 + 8) / 4];
    const u32 original_data = config.words[(0x50 + 12) / 4];
    const u32 original_mask = config.words[(0x50 + 16) / 4];

    pci::msi_capability capability{};
    REQUIRE(find_msi(config, capability));
    const pci::msi_config_ops operations{read_config, write_config, &config};
    const pci::msi_message message{0xfee00000ull | (0x35ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msi_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;

    REQUIRE(pci::prepare_remappable_msi(operations, capability, message, previous, failure));
    REQUIRE(pci::set_remappable_msi_enabled(operations, previous, true, failure));
    REQUIRE(pci::set_remappable_msi_enabled(operations, previous, false, failure));
    REQUIRE(pci::restore_msi_configuration(operations, previous, failure));
    CHECK(failure == pci::msi_config_error::none);
    CHECK(config.words[0x50 / 4] == original_header);
    CHECK(config.words[(0x50 + 4) / 4] == original_address_low);
    CHECK(config.words[(0x50 + 8) / 4] == original_address_high);
    CHECK(config.words[(0x50 + 12) / 4] == original_data);
    CHECK(config.words[(0x50 + 16) / 4] == original_mask);
}

TEST_CASE("PCI MSI enable and restore reject a changed remappable destination", "[pci][msi]")
{
    fake_config config{};
    set_msi_capability(config, 0x50, true, true, 1);
    pci::msi_capability capability{};
    REQUIRE(find_msi(config, capability));
    const pci::msi_config_ops operations{read_config, write_config, &config};
    const pci::msi_message message{0xfee00000ull | (0x35ull << 5) | (1ull << 4) | (1ull << 3), 0};
    pci::msi_config_snapshot previous{};
    pci::msi_config_error failure = pci::msi_config_error::none;

    REQUIRE(pci::prepare_remappable_msi(operations, capability, message, previous, failure));
    const u32 writes_after_prepare = config.writes;
    config.words[(0x50 + 4) / 4] ^= 1u << 12;

    CHECK_FALSE(pci::set_remappable_msi_enabled(operations, previous, true, failure));
    CHECK(failure == pci::msi_config_error::invalid_message);
    CHECK(config.writes == writes_after_prepare);

    config.words[(0x50 + 4) / 4] = static_cast<u32>(message.address);
    REQUIRE(pci::set_remappable_msi_enabled(operations, previous, true, failure));
    REQUIRE(pci::set_remappable_msi_enabled(operations, previous, false, failure));
    // Restore must not overwrite a destination changed by another config
    // writer while this route was prepared.
    config.words[(0x50 + 4) / 4] ^= 1u << 12;
    const u32 writes_before_restore = config.writes;
    CHECK_FALSE(pci::restore_msi_configuration(operations, previous, failure));
    CHECK(failure == pci::msi_config_error::invalid_message);
    CHECK(config.writes == writes_before_restore);
}

TEST_CASE("PCI MSI teardown drains on both sides of IRTE invalidation", "[pci][msi]")
{
    fake_teardown fake{};
    const pci::msi_route_teardown_ops operations{disable_route, drain_route, retire_route_irte, restore_route, &fake};

    const auto result = pci::teardown_msi_route(operations);

    CHECK(result.failure == pci::msi_route_teardown_failure::none);
    CHECK_FALSE(result.outcome_uncertain);
    CHECK(result.vector_recyclable);
    REQUIRE(fake.operation_count == 5);
    CHECK(fake.operations[0] == teardown_operation::disable);
    CHECK(fake.operations[1] == teardown_operation::drain);
    CHECK(fake.operations[2] == teardown_operation::retire_irte);
    CHECK(fake.operations[3] == teardown_operation::drain);
    CHECK(fake.operations[4] == teardown_operation::restore);
}

TEST_CASE("PCI MSI teardown stops at the first uncertain transition", "[pci][msi]")
{
    for (u32 failed_position = 0; failed_position < 5; ++failed_position)
    {
        fake_teardown fake{};
        fake.fail_at = failed_position;
        const pci::msi_route_teardown_ops operations{disable_route, drain_route, retire_route_irte, restore_route,
                                                     &fake};

        const auto result = pci::teardown_msi_route(operations);

        CHECK(result.failure != pci::msi_route_teardown_failure::none);
        CHECK(result.outcome_uncertain);
        CHECK(result.vector_recyclable == (failed_position == 4));
        CHECK(fake.operation_count == failed_position + 1);
    }
}

TEST_CASE("PCI MSI route registry allows one live route per physical function", "[pci][msi]")
{
    pci::msi_route_registry<2> registry;
    const pci::msi_route_function_key function{0, 4, 2, 1};
    const pci::msi_route_function_key other_function{0, 4, 3, 0};

    CHECK(registry.reserve(function) == pci::msi_route_registry_error::none);
    CHECK(registry.reserve(function) == pci::msi_route_registry_error::duplicate);
    CHECK(registry.reserve(other_function) == pci::msi_route_registry_error::none);
    CHECK(registry.release(function) == pci::msi_route_registry_error::none);
    CHECK(registry.reserve(function) == pci::msi_route_registry_error::none);
}

TEST_CASE("uncertain PCI MSI routes keep their function identity quarantined", "[pci][msi]")
{
    pci::msi_route_registry<1> registry;
    const pci::msi_route_function_key function{2, 9, 31, 7};
    const pci::msi_route_function_key other_function{2, 9, 31, 6};

    REQUIRE(registry.reserve(function) == pci::msi_route_registry_error::none);
    REQUIRE(registry.quarantine(function) == pci::msi_route_registry_error::none);
    CHECK(registry.release(function) == pci::msi_route_registry_error::already_quarantined);
    CHECK(registry.reserve(function) == pci::msi_route_registry_error::already_quarantined);
    CHECK(registry.reserve(other_function) == pci::msi_route_registry_error::full);
}

TEST_CASE("PCI interrupt event acknowledgements preserve coalesced arrivals", "[pci][msi]")
{
    pci::interrupt_event_state event;
    CHECK_FALSE(event.readable());
    CHECK(event.interrupt());
    const u64 first_sequence = event.sequence();
    CHECK(event.readable());
    CHECK_FALSE(event.interrupt());
    CHECK(event.sequence() != first_sequence);
    CHECK(event.acknowledge(first_sequence) == pci::interrupt_event_ack::stale_sequence);
    const u64 latest_sequence = event.sequence();
    CHECK(event.acknowledge(latest_sequence) == pci::interrupt_event_ack::acknowledged);
    CHECK_FALSE(event.readable());
    CHECK(event.acknowledge(latest_sequence) == pci::interrupt_event_ack::not_pending);
}
