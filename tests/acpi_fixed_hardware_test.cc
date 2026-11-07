#include <catch2/catch_test_macros.hpp>

#include "kernel/acpi/event_enable.hpp"
#include "kernel/acpi/fixed_inventory.hpp"
#include "kernel/acpi/global_lock.hpp"
#include "kernel/acpi/sci_lifecycle.hpp"
#include "kernel/acpi/sci_owner.hpp"
#include "kernel/acpi/sci_status.hpp"
#include "kernel/acpi/transition_lifecycle.hpp"
#include "kernel/physical_resource.hpp"

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace
{
class host_allocator final : public freelibcxx::Allocator
{
  public:
    void *allocate(size_t size, size_t align) noexcept override
    {
        (void)align;
        return std::malloc(size);
    }

    void deallocate(void *ptr) noexcept override { std::free(ptr); }
};

void write_u32(u8 *bytes, size_t offset, u32 value)
{
    for (size_t index = 0; index < sizeof(value); ++index)
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
}

void write_gas(u8 *bytes, size_t offset, u8 space, u8 width, u64 address, u8 bit_offset = 0, u8 access_size = 0)
{
    bytes[offset] = space;
    bytes[offset + 1] = width;
    bytes[offset + 2] = bit_offset;
    bytes[offset + 3] = access_size;
    for (size_t index = 0; index < sizeof(address); ++index)
        bytes[offset + 4 + index] = static_cast<u8>(address >> (index * 8));
}
} // namespace

namespace arch::idt
{
void enable() {}
void disable() {}
bool save_and_disable() { return true; }
} // namespace arch::idt

TEST_CASE("FACS Global Lock transitions preserve firmware bits and implement Pending and Owned", "[acpi_global_lock]")
{
    constexpr u32 reserved_bit = 1u << 31;

    const auto first_acquire = acpi::next_global_lock_acquire(reserved_bit);
    CHECK(first_acquire.acquired);
    CHECK(first_acquire.value == (reserved_bit | acpi::facs_global_lock_owned));

    const auto contended_acquire = acpi::next_global_lock_acquire(reserved_bit | acpi::facs_global_lock_owned);
    CHECK_FALSE(contended_acquire.acquired);
    CHECK(contended_acquire.value == (reserved_bit | acpi::facs_global_lock_owned | acpi::facs_global_lock_pending));

    const auto stale_pending_acquire = acpi::next_global_lock_acquire(acpi::facs_global_lock_pending);
    CHECK(stale_pending_acquire.acquired);
    CHECK(stale_pending_acquire.value == acpi::facs_global_lock_owned);

    const auto pending_release =
        acpi::next_global_lock_release(reserved_bit | acpi::facs_global_lock_owned | acpi::facs_global_lock_pending);
    CHECK(pending_release.owned);
    CHECK(pending_release.notify_firmware);
    CHECK(pending_release.value == reserved_bit);

    const auto unowned_release = acpi::next_global_lock_release(reserved_bit | acpi::facs_global_lock_pending);
    CHECK_FALSE(unowned_release.owned);
    CHECK_FALSE(unowned_release.notify_firmware);
    CHECK(unowned_release.value == (reserved_bit | acpi::facs_global_lock_pending));

    constexpr u64 pm1_control = (1u << 0) | (1u << 9) | (5u << 10);
    CHECK(acpi::next_pm1_global_lock_release(pm1_control) == (pm1_control | acpi::pm1_global_lock_release));
}

TEST_CASE("Software Global Lock works without SCI while FACS arbitration stays fail-closed", "[acpi_global_lock]")
{
    using path = acpi::global_lock_arbitration_path;

    CHECK(acpi::select_global_lock_arbitration_path(false, false, false, false) == path::software);
    CHECK(acpi::select_global_lock_arbitration_path(false, false, false, true) == path::software);
    CHECK(acpi::select_global_lock_arbitration_path(true, true, false, false) == path::software);
    CHECK(acpi::select_global_lock_arbitration_path(true, true, false, true) == path::software);
    CHECK(acpi::select_global_lock_arbitration_path(true, false, false, false) == path::facs_poll);
    CHECK(acpi::select_global_lock_arbitration_path(true, false, true, false) == path::facs_event);
    CHECK(acpi::select_global_lock_arbitration_path(true, false, false, true) == path::fail_closed);
    CHECK(acpi::select_global_lock_arbitration_path(true, false, true, true) == path::fail_closed);
}

TEST_CASE("Only loss of an enabled Global Lock SCI path quarantines FACS arbitration", "[acpi_global_lock]")
{
    CHECK_FALSE(acpi::global_lock_event_lost_after_failure(false, false));
    CHECK(acpi::global_lock_event_lost_after_failure(true, false));
    CHECK(acpi::global_lock_event_lost_after_failure(false, true));
}

TEST_CASE("Kernel Global Lock owner state rejects duplicate and contended leases", "[acpi_global_lock]")
{
    using decision = acpi::global_lock_owner_claim_decision;

    CHECK(acpi::classify_global_lock_owner_claim(0, 0, false, false, 17, 31) == decision::claim);
    CHECK(acpi::classify_global_lock_owner_claim(17, 31, false, true, 17, 31) == decision::already_owned);
    CHECK(acpi::classify_global_lock_owner_claim(17, 31, true, false, 17, 31) == decision::already_owned);
    CHECK(acpi::classify_global_lock_owner_claim(17, 31, false, true, 18, 32) == decision::contended);
    CHECK(acpi::classify_global_lock_owner_claim(17, 31, false, false, 17, 32) == decision::contended);
    CHECK(acpi::classify_global_lock_owner_claim(17, 31, false, false, 18, 31) == decision::contended);
    CHECK(acpi::classify_global_lock_owner_claim(17, 31, true, true, 18, 32) == decision::invalid_state);
    CHECK(acpi::classify_global_lock_owner_claim(0, 31, false, false, 17, 31) == decision::invalid_state);
    CHECK(acpi::classify_global_lock_owner_claim(0, 0, true, false, 17, 31) == decision::invalid_state);
    CHECK(acpi::classify_global_lock_owner_claim(0, 0, false, true, 17, 31) == decision::invalid_state);
    CHECK(acpi::classify_global_lock_owner_claim(0, 0, false, false, 0, 31) == decision::invalid_state);
}

TEST_CASE("PM1 control register updates serialize Global Lock release and sleep writes", "[acpi_fixed_hardware]")
{
    std::mutex update_lock;
    std::atomic<u32> inside_update{0};
    std::atomic_bool overlapped{false};
    std::barrier start(3);
    const auto update = [&] {
        start.arrive_and_wait();
        for (u32 iteration = 0; iteration < 2000; ++iteration)
        {
            acpi::with_pm1_control_update_lock<std::lock_guard<std::mutex>>(update_lock, [&] {
                if (inside_update.fetch_add(1, std::memory_order_acq_rel) != 0)
                    overlapped.store(true, std::memory_order_release);
                std::this_thread::yield();
                inside_update.fetch_sub(1, std::memory_order_acq_rel);
            });
        }
    };

    std::thread release_update(update);
    std::thread sleep_update(update);
    start.arrive_and_wait();
    release_update.join();
    sleep_update.join();
    CHECK_FALSE(overlapped.load(std::memory_order_acquire));
}

void get_task_id(u64 &, u64 &) {}

TEST_CASE("FADT extended PM1 and GPE GAS registers become fixed inventory and reserved ranges", "[acpi_fixed_hardware]")
{
    u8 fadt[244]{};
    write_u32(fadt, 56, 0);
    write_u32(fadt, 60, 0);
    write_u32(fadt, 64, 0);
    write_u32(fadt, 68, 0);
    write_u32(fadt, 80, 0);
    write_u32(fadt, 84, 0);
    fadt[88] = 4;
    fadt[89] = 2;
    fadt[92] = 4;
    fadt[93] = 4;
    fadt[94] = 0x40;
    // FADT block GAS entries rely on the legacy block-length fields; firmware
    // may leave BitWidth zero for these descriptors.
    write_gas(fadt, 148, 0, 0, 0xFED80000);
    write_gas(fadt, 172, 0, 0, 0xFED80100);
    write_gas(fadt, 220, 1, 0, 0x1800);
    write_gas(fadt, 232, 1, 0, 0x1820);

    acpi::fadt_fixed_inventory parsed{};
    REQUIRE(acpi::parse_fadt_fixed_inventory(fadt, sizeof(fadt), 7, 11, parsed));
    CHECK(parsed.active.boot_epoch == 7);
    CHECK(parsed.active.firmware_generation == 11);
    CHECK(parsed.active.pm1_event.address == 0xFED80000);
    CHECK(parsed.active.pm1_event.space == 0);
    CHECK(parsed.active.pm1_event.length == 2);
    CHECK(parsed.active.pm1_event.access_width == 2);
    CHECK(parsed.active.pm1_event_enable.address == 0xFED80002);
    CHECK(parsed.active.pm1_event_enable.length == 2);
    CHECK(parsed.active.pm1_event_enable.access_width == 2);
    CHECK(parsed.active.pm1_control.address == 0xFED80100);
    CHECK(parsed.active.pm1_control.space == 0);
    CHECK(parsed.active.pm1_control.length == 2);
    CHECK(parsed.active.gpe0_status.address == 0x1800);
    CHECK(parsed.active.gpe0_status.space == 1);
    CHECK(parsed.active.gpe0_status.length == 2);
    CHECK(parsed.active.gpe0_status.access_width == 2);
    CHECK(parsed.active.gpe0_enable.address == 0x1802);
    CHECK(parsed.active.gpe0_enable.length == 2);
    CHECK(parsed.active.gpe1_base == 0x40);
    CHECK(parsed.active.gpe1_status.address == 0x1820);
    CHECK(parsed.active.gpe1_enable.address == 0x1822);

    const auto is_reserved = [&](u64 address, u32 length, u8 space) {
        for (u32 index = 0; index < parsed.reserved_count; ++index)
        {
            const auto &range = parsed.reserved[index];
            if (range.space == space && range.address <= address && length <= range.length &&
                address - range.address <= range.length - length)
                return true;
        }
        return false;
    };
    CHECK(is_reserved(0xFED80000, 4, 0));
    CHECK(is_reserved(0xFED80100, 2, 0));
    CHECK(is_reserved(0x1800, 4, 1));

    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::root authority(&ledger);
    for (u32 index = 0; index < parsed.reserved_count; ++index)
    {
        const auto &range = parsed.reserved[index];
        const u64 end = range.address + range.length;
        const u64 page_base = range.address & ~u64{4095};
        const u64 page_end = (end + 4095) & ~u64{4095};
        const auto kind =
            range.space == 1 ? physical_resource::kind::io_port : physical_resource::kind::acpi_fixed_hardware;
        const auto access =
            range.space == 1 ? physical_resource::access::access : physical_resource::access::read_write;
        physical_resource::lease lease{};
        REQUIRE(ledger.reserve(authority, range.address, range.length, page_base, page_end, kind,
                               physical_resource::cache::uncacheable, access, 11, lease));
    }
    const auto denied_by_ledger = [&](const acpi::fixed_register &reg) {
        const u64 end = reg.address + reg.length;
        const u64 page_base = reg.address & ~u64{4095};
        const u64 page_end = (end + 4095) & ~u64{4095};
        const auto kind =
            reg.space == 1 ? physical_resource::kind::io_port : physical_resource::kind::acpi_fixed_hardware;
        const auto access = reg.space == 1 ? physical_resource::access::access : physical_resource::access::read_write;
        physical_resource::lease lease{};
        return !ledger.reserve(authority, reg.address, reg.length, page_base, page_end, kind,
                               physical_resource::cache::uncacheable, access, 11, lease);
    };
    CHECK(denied_by_ledger(parsed.active.pm1_event));
    CHECK(denied_by_ledger(parsed.active.pm1_control));
    CHECK(denied_by_ledger(parsed.active.gpe0_status));
}

TEST_CASE("Malformed non-empty FADT extended GAS fails closed", "[acpi_fixed_hardware]")
{
    u8 fadt[232]{};
    fadt[88] = 4;
    write_gas(fadt, 148, 0, 32, 0xFED80000);
    write_gas(fadt, 220, 2, 32, 0xFED80100);

    acpi::fadt_fixed_inventory parsed{};
    CHECK_FALSE(acpi::parse_fadt_fixed_inventory(fadt, sizeof(fadt), 1, 1, parsed));
}

TEST_CASE("FADT GAS bit offset and access size are honored", "[acpi_fixed_hardware]")
{
    u8 fadt[244]{};
    fadt[88] = 4;
    fadt[89] = 2;
    write_gas(fadt, 148, 0, 32, 0xFED80000, 0, 2); // two 16-bit PM1 event registers
    write_gas(fadt, 172, 0, 16, 0xFED80100, 0, 2); // 16-bit PM1 control
    write_gas(fadt, 116, 0, 8, 0xFED80200, 0, 2);  // 8-bit field accessed as 16-bit

    acpi::fadt_fixed_inventory parsed{};
    REQUIRE(acpi::parse_fadt_fixed_inventory(fadt, sizeof(fadt), 1, 1, parsed));

    CHECK(parsed.active.pm1_event.length == 2);
    CHECK(parsed.active.pm1_event.gas_bit_offset == 0);
    CHECK(parsed.active.pm1_event.gas_access_size == 2);
    CHECK(parsed.active.pm1_event.access_width == 2);
    CHECK(parsed.active.pm1_control.access_width == 2);
    CHECK(parsed.active.reset.length == 2);
    CHECK(parsed.active.reset.gas_access_size == 2);
    CHECK(parsed.active.reset.access_width == 2);
}

TEST_CASE("SCI ownership masks all fixed event enables before userspace dispatch", "[acpi_fixed_hardware]")
{
    acpi::fixed_inventory inventory{};
    inventory.pm1_event_enable = {0x602, 2, 1, 0, 0, 2};
    inventory.pm1_event_b_enable = {0x606, 2, 1, 0, 0, 2};
    inventory.gpe0_enable = {0xAFE4, 8, 1, 0, 0, 4};
    inventory.gpe1_enable = {0xAFE8, 8, 1, 0, 0, 4};

    u32 writes = 0;
    const bool masked =
        acpi::disable_acpi_event_sources(inventory, [&writes](const acpi::fixed_register &reg, u64 value) {
            ++writes;
            CHECK(value == 0);
            CHECK(reg.length != 0);
            CHECK((reg.address == 0x602 || reg.address == 0x606 || reg.address == 0xAFE4 || reg.address == 0xAFE8));
            return true;
        });

    CHECK(masked);
    CHECK(writes == 4);

    writes = 0;
    const bool partial_failure =
        acpi::disable_acpi_event_sources(inventory, [&writes](const acpi::fixed_register &, u64) {
            ++writes;
            return writes != 3;
        });
    CHECK_FALSE(partial_failure);
    CHECK(writes == 4);
}

TEST_CASE("Q35-style FADT GAS ranges select aligned scalar access widths", "[acpi_fixed_hardware]")
{
    u8 fadt[276]{};
    fadt[88] = 4;
    fadt[89] = 2;
    fadt[91] = 4;
    fadt[92] = 16;
    write_gas(fadt, 148, 1, 32, 0x600);
    write_gas(fadt, 172, 1, 16, 0x604);
    write_gas(fadt, 208, 1, 32, 0x608);
    write_gas(fadt, 220, 1, 128, 0xAFE0);
    write_gas(fadt, 116, 1, 8, 0xCF9);

    acpi::fadt_fixed_inventory parsed{};
    REQUIRE(acpi::parse_fadt_fixed_inventory(fadt, sizeof(fadt), 1, 1, parsed));
    CHECK(parsed.active.pm1_event.access_width == 2);
    CHECK(parsed.active.pm1_event_enable.address == 0x602);
    CHECK(parsed.active.pm1_event_enable.access_width == 2);
    CHECK(parsed.active.pm1_control.access_width == 2);
    CHECK(parsed.active.gpe0_status.access_width == 4);
    CHECK(parsed.active.gpe0_enable.access_width == 4);
    CHECK(parsed.active.reset.access_width == 1);
}

TEST_CASE("FADT fixed GAS rejects offsets and access widths that cannot be honored", "[acpi_fixed_hardware]")
{
    const auto parse_reset_gas = [](u8 bit_offset, u8 access_size, u64 address) {
        u8 fadt[128]{};
        write_gas(fadt, 116, 0, 8, address, bit_offset, access_size);
        acpi::fadt_fixed_inventory parsed{};
        return acpi::parse_fadt_fixed_inventory(fadt, sizeof(fadt), 1, 1, parsed);
    };

    CHECK_FALSE(parse_reset_gas(1, 1, 0xFED80200));
    CHECK_FALSE(parse_reset_gas(0, 5, 0xFED80200));
    CHECK_FALSE(parse_reset_gas(0, 2, 0xFED80201));

    u8 fadt[244]{};
    fadt[88] = 4;
    write_gas(fadt, 148, 0, 32, 0xFED80000, 0, 3); // one dword would span status and enable
    acpi::fadt_fixed_inventory parsed{};
    CHECK_FALSE(acpi::parse_fadt_fixed_inventory(fadt, sizeof(fadt), 1, 1, parsed));
}

TEST_CASE("SCI status clearing is limited to the current drained sequence and observed bits", "[acpi_fixed_hardware]")
{
    acpi::sci_status_ledger ledger;
    const acpi::sci_status_masks observed{0x04, 0, 0x20, 0x02};
    const acpi::sci_status_masks handled{0, 0, 0x20, 0};
    const acpi::sci_status_masks remaining{0x04, 0, 0, 0x02};
    const acpi::sci_status_masks reasserted{0x04, 0, 0, 0x06};

    CHECK(ledger.record(17, observed));
    CHECK(ledger.validate_clear(16, handled) == acpi::sci_status_clear_error::stale_sequence);
    CHECK(ledger.validate_clear(17, acpi::sci_status_masks{0x08, 0, 0, 0}) ==
          acpi::sci_status_clear_error::unobserved_bits);
    CHECK(ledger.validate_clear(17, handled) == acpi::sci_status_clear_error::none);
    ledger.commit_clear(17, handled);
    CHECK(ledger.remaining(17) == remaining);
    CHECK(ledger.validate_clear(17, handled) == acpi::sci_status_clear_error::already_cleared);
    REQUIRE(ledger.record(17, acpi::sci_status_masks{0, 0, 0, 0x04}));
    CHECK(ledger.remaining(17) == reasserted);

    ledger.invalidate();
    CHECK(ledger.validate_clear(17, acpi::sci_status_masks{0x04, 0, 0, 0}) == acpi::sci_status_clear_error::no_drain);
}

TEST_CASE("FADT extended GAS wins over legacy access while both aliases remain reserved", "[acpi_fixed_hardware]")
{
    u8 fadt[244]{};
    write_u32(fadt, 56, 0x400);
    write_u32(fadt, 64, 0x404);
    fadt[88] = 4;
    fadt[89] = 2;
    write_gas(fadt, 148, 0, 32, 0xFED80000);
    write_gas(fadt, 172, 0, 16, 0xFED80100);

    acpi::fadt_fixed_inventory parsed{};
    REQUIRE(acpi::parse_fadt_fixed_inventory(fadt, sizeof(fadt), 3, 5, parsed));
    CHECK(parsed.active.pm1_event.address == 0xFED80000);
    CHECK(parsed.active.pm1_event.space == 0);
    CHECK(parsed.active.pm1_control.address == 0xFED80100);
    CHECK(parsed.active.pm1_control.space == 0);

    const auto is_reserved = [&](u64 address, u32 length, u8 space) {
        for (u32 index = 0; index < parsed.reserved_count; ++index)
        {
            const auto &range = parsed.reserved[index];
            if (range.space == space && range.address <= address && length <= range.length &&
                address - range.address <= range.length - length)
                return true;
        }
        return false;
    };
    CHECK(is_reserved(0x400, 4, 1));
    CHECK(is_reserved(0x404, 2, 1));
    CHECK(is_reserved(0xFED80000, 4, 0));
    CHECK(is_reserved(0xFED80100, 2, 0));
}

TEST_CASE("SCI ownership is exclusive, idempotent, and permanently revoked on owner exit", "[acpi_fixed_hardware]")
{
    acpi::fixed_hardware_owner_lifecycle lifecycle;

    CHECK_FALSE(lifecycle.claim(0));
    REQUIRE(lifecycle.claim(41));
    CHECK(lifecycle.claim(41));
    CHECK(lifecycle.owns(41));
    CHECK_FALSE(lifecycle.owns(42));
    CHECK_FALSE(lifecycle.claim(42));
    CHECK_FALSE(lifecycle.owner_exit(42));
    CHECK(lifecycle.owns(41));

    CHECK(lifecycle.owner_exit(41));
    CHECK(lifecycle.revoked());
    CHECK_FALSE(lifecycle.owns(41));
    CHECK(lifecycle.revoked_for(41));
    CHECK_FALSE(lifecycle.revoked_for(42));
    CHECK_FALSE(lifecycle.claim(42));
    CHECK_FALSE(lifecycle.owner_exit(41));
}

TEST_CASE("fixed-hardware transition ownership is independent from SCI event ownership", "[acpi_fixed_hardware]")
{
    acpi::fixed_hardware_owner_lifecycle transition_owner;
    acpi::fixed_hardware_owner_lifecycle sci_owner;

    REQUIRE(transition_owner.claim(41));
    CHECK(transition_owner.owns(41));
    CHECK_FALSE(sci_owner.owns(41));

    REQUIRE(sci_owner.claim(41));
    CHECK(sci_owner.owner_exit(41));
    CHECK(sci_owner.revoked_for(41));
    CHECK(transition_owner.owns(41));

    CHECK(transition_owner.owner_exit(41));
    CHECK(transition_owner.revoked_for(41));
}

TEST_CASE("SCI events mask, coalesce, drain, and unmask only after a matching acknowledgement", "[acpi_fixed_hardware]")
{
    acpi::sci_event_lifecycle lifecycle;

    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::suppressed);
    REQUIRE(lifecycle.claim_source());
    CHECK(lifecycle.state_value() == 0);

    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);
    const auto first_sequence = lifecycle.sequence();
    CHECK(first_sequence == 1);
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::coalesced_and_signaled);
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::coalesced_and_signaled);

    u32 drain_flags = 0;
    REQUIRE(lifecycle.begin_drain(first_sequence) == acpi::sci_drain_result::ready);
    REQUIRE(lifecycle.finish_drain(first_sequence, drain_flags) == acpi::sci_drain_result::ready);
    CHECK(drain_flags == 3);
    CHECK_FALSE(lifecycle.event_ready());

    u32 state = 99;
    u32 flags = 99;
    CHECK(lifecycle.acknowledge(first_sequence, false, state, flags) == acpi::sci_ack_result::rearmed);
    CHECK(state == 0);
    CHECK(flags == 1);
    CHECK(lifecycle.state_value() == 0);
    CHECK_FALSE(lifecycle.event_ready());
    CHECK(lifecycle.acknowledge(first_sequence, false, state, flags) == acpi::sci_ack_result::conflict);
    CHECK(lifecycle.state_value() == 2);
}

TEST_CASE("fixed transitions allow idle or degraded SCI state but reject an in-flight event", "[acpi_fixed_hardware]")
{
    acpi::sci_event_lifecycle lifecycle;
    CHECK(lifecycle.fixed_transition_ready()); // SCI remains masked until an event owner claims it.

    REQUIRE(lifecycle.claim_source());
    CHECK(lifecycle.fixed_transition_ready());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);
    CHECK_FALSE(lifecycle.fixed_transition_ready());

    lifecycle.degrade();
    CHECK(lifecycle.fixed_transition_ready()); // commit performs a fresh hardware mask and may fail closed.
}

TEST_CASE("SCI acknowledgement resignal advances sequence and stale acknowledgement degrades masked source",
          "[acpi_fixed_hardware]")
{
    acpi::sci_event_lifecycle lifecycle;
    REQUIRE(lifecycle.claim_source());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);
    const auto first_sequence = lifecycle.sequence();

    u32 flags = 0;
    REQUIRE(lifecycle.begin_drain(first_sequence) == acpi::sci_drain_result::ready);
    u32 drain_flags = 0;
    REQUIRE(lifecycle.finish_drain(first_sequence, drain_flags) == acpi::sci_drain_result::ready);

    u32 state = 99;
    CHECK(lifecycle.acknowledge(first_sequence, true, state, flags) == acpi::sci_ack_result::resignaled);
    CHECK(state == 1);
    CHECK(flags == 2);
    CHECK(lifecycle.sequence() != first_sequence);
    CHECK(lifecycle.event_ready());
    CHECK(lifecycle.state_value() == 1);

    CHECK(lifecycle.acknowledge(first_sequence, false, state, flags) == acpi::sci_ack_result::stale_sequence);
    CHECK(lifecycle.state_value() == 2);
    CHECK(lifecycle.readable());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::suppressed);
    CHECK(lifecycle.acknowledge(lifecycle.sequence(), false, state, flags) == acpi::sci_ack_result::degraded);
}

TEST_CASE("SCI event sequence exhaustion degrades instead of reusing an old sequence", "[acpi_fixed_hardware]")
{
    constexpr u64 last_sequence = ~u64{0};
    acpi::sci_event_lifecycle lifecycle(last_sequence);
    REQUIRE(lifecycle.claim_source());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);
    CHECK(lifecycle.sequence() == last_sequence);

    u32 drain_flags = 0;
    REQUIRE(lifecycle.begin_drain(last_sequence) == acpi::sci_drain_result::ready);
    REQUIRE(lifecycle.finish_drain(last_sequence, drain_flags) == acpi::sci_drain_result::ready);
    u32 state = 0;
    u32 flags = 0;
    CHECK(lifecycle.acknowledge(last_sequence, false, state, flags) == acpi::sci_ack_result::rearmed);

    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::degraded);
    CHECK(lifecycle.state_value() == 2);
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::suppressed);
}

TEST_CASE("SCI resignal exhaustion leaves the source degraded and masked", "[acpi_fixed_hardware]")
{
    constexpr u64 last_sequence = ~u64{0};
    acpi::sci_event_lifecycle lifecycle(last_sequence);
    REQUIRE(lifecycle.claim_source());
    REQUIRE(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);
    REQUIRE(lifecycle.begin_drain(last_sequence) == acpi::sci_drain_result::ready);
    u32 drain_flags = 0;
    REQUIRE(lifecycle.finish_drain(last_sequence, drain_flags) == acpi::sci_drain_result::ready);

    u32 state = 0;
    u32 flags = 0;
    CHECK(lifecycle.acknowledge(last_sequence, true, state, flags) == acpi::sci_ack_result::degraded);
    CHECK(state == 2);
    CHECK(flags == 1);
    CHECK(lifecycle.state_value() == 2);
}

TEST_CASE("SCI drain rejects stale sequences without clearing the pending event", "[acpi_fixed_hardware]")
{
    acpi::sci_event_lifecycle lifecycle;
    REQUIRE(lifecycle.claim_source());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);
    const auto sequence = lifecycle.sequence();

    u32 flags = 0;
    CHECK(lifecycle.begin_drain(sequence + 1) == acpi::sci_drain_result::stale_sequence);
    CHECK(lifecycle.event_ready());
    REQUIRE(lifecycle.begin_drain(sequence) == acpi::sci_drain_result::ready);
    REQUIRE(lifecycle.finish_drain(sequence, flags) == acpi::sci_drain_result::ready);
    CHECK(flags == 0);
}

TEST_CASE("SCI drain classifies stale sequence separately from a missing event", "[acpi_fixed_hardware]")
{
    acpi::sci_event_lifecycle lifecycle;
    REQUIRE(lifecycle.claim_source());
    CHECK(lifecycle.begin_drain(0) == acpi::sci_drain_result::would_block);

    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);
    const auto sequence = lifecycle.sequence();
    CHECK(lifecycle.begin_drain(sequence + 1) == acpi::sci_drain_result::stale_sequence);
    CHECK(lifecycle.begin_drain(sequence) == acpi::sci_drain_result::ready);

    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::stale_generation) == ESTALE);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::resource_revoked) == EIDRM);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::would_block) == EAGAIN);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::access_denied) == EACCES);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::conflict) == EBUSY);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::io_error) == EIO);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::invalid_argument) == EINVAL);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::not_supported) == ENOTSUP);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::not_ready) == ENXIO);
    CHECK(acpi::sci_operation_errno(acpi::sci_operation_result::resource_exhausted) == ENOMEM);

    lifecycle.degrade();
    CHECK(lifecycle.begin_drain(sequence) == acpi::sci_drain_result::resource_revoked);
}

TEST_CASE("SCI owner loss makes event delivery permanently fail closed", "[acpi_fixed_hardware]")
{
    acpi::sci_event_lifecycle lifecycle;
    REQUIRE(lifecycle.claim_source());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);

    lifecycle.degrade();
    CHECK(lifecycle.state_value() == 2);
    CHECK(lifecycle.readable());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::suppressed);
    CHECK_FALSE(lifecycle.claim_source());
}

TEST_CASE("SCI rearm failure becomes sticky and never resumes interrupt delivery", "[acpi_fixed_hardware]")
{
    acpi::sci_event_lifecycle lifecycle;
    REQUIRE(lifecycle.claim_source());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::masked_and_signaled);
    const auto sequence = lifecycle.sequence();

    u32 drain_flags = 0;
    REQUIRE(lifecycle.begin_drain(sequence) == acpi::sci_drain_result::ready);
    REQUIRE(lifecycle.finish_drain(sequence, drain_flags) == acpi::sci_drain_result::ready);
    u32 state = 99;
    u32 flags = 99;
    CHECK(lifecycle.acknowledge(sequence, false, state, flags) == acpi::sci_ack_result::rearmed);

    lifecycle.rearm_failed();
    CHECK(lifecycle.state_value() == 2);
    CHECK(lifecycle.readable());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::suppressed);

    lifecycle.rearm_failed();
    CHECK(lifecycle.state_value() == 2);
    CHECK(lifecycle.readable());
    CHECK(lifecycle.interrupt() == acpi::sci_interrupt_action::suppressed);
}

TEST_CASE("ACPI transition tickets can be cancelled and cannot be replayed", "[acpi_fixed_hardware]")
{
    acpi::transition_lifecycle lifecycle;
    u64 ticket = 0;

    REQUIRE(lifecycle.prepare(acpi::transition_kind::sleep, 5, 5, 0, true, ticket) ==
            acpi::transition_prepare_result::prepared);
    CHECK(ticket == 1);
    CHECK(lifecycle.prepare(acpi::transition_kind::reset, 0, 0, 0, true, ticket) ==
          acpi::transition_prepare_result::conflict);
    CHECK_FALSE(lifecycle.cancel(ticket + 1, 0));
    CHECK(lifecycle.cancel(ticket, 0));
    CHECK_FALSE(lifecycle.cancel(ticket, 0));

    u64 next_ticket = 0;
    REQUIRE(lifecycle.prepare(acpi::transition_kind::reset, 0, 0, 0, true, next_ticket) ==
            acpi::transition_prepare_result::prepared);
    CHECK(next_ticket != ticket);
    u32 sleep_type_a = 99;
    u32 sleep_type_b = 99;
    CHECK_FALSE(lifecycle.begin_commit(acpi::transition_kind::sleep, next_ticket, 0, sleep_type_a, sleep_type_b));
    CHECK(lifecycle.begin_commit(acpi::transition_kind::reset, next_ticket, 0, sleep_type_a, sleep_type_b));
    CHECK(sleep_type_a == 0);
    CHECK(sleep_type_b == 0);
    CHECK_FALSE(lifecycle.cancel(next_ticket, 0));
    CHECK(lifecycle.committing());
    lifecycle.finish();
    CHECK_FALSE(lifecycle.active(0));
    CHECK_FALSE(lifecycle.begin_commit(acpi::transition_kind::reset, next_ticket, 0, sleep_type_a, sleep_type_b));
}

TEST_CASE("ACPI transition owner exit releases a prepared ticket", "[acpi_fixed_hardware]")
{
    acpi::transition_lifecycle lifecycle;
    u64 ticket = 0;
    REQUIRE(lifecycle.prepare(acpi::transition_kind::sleep, 5, 5, 0, true, ticket) ==
            acpi::transition_prepare_result::prepared);

    lifecycle.owner_exit();

    CHECK_FALSE(lifecycle.active(0));
    CHECK_FALSE(lifecycle.cancel(ticket, 0));
}

TEST_CASE("ACPI sleep transition preserves its prepared sleep types through commit", "[acpi_fixed_hardware]")
{
    acpi::transition_lifecycle lifecycle;
    u64 ticket = 0;
    REQUIRE(lifecycle.prepare(acpi::transition_kind::sleep, 5, 6, 100, true, ticket) ==
            acpi::transition_prepare_result::prepared);

    u32 sleep_type_a = 0;
    u32 sleep_type_b = 0;
    REQUIRE(lifecycle.begin_commit(acpi::transition_kind::sleep, ticket, 101, sleep_type_a, sleep_type_b));
    CHECK(sleep_type_a == 5);
    CHECK(sleep_type_b == 6);
    CHECK_FALSE(lifecycle.begin_commit(acpi::transition_kind::sleep, ticket, 101, sleep_type_a, sleep_type_b));
}

TEST_CASE("ACPI transition ticket exhaustion fails closed instead of reusing tickets", "[acpi_fixed_hardware]")
{
    acpi::transition_lifecycle lifecycle(~u64{0});
    u64 ticket = 0;

    REQUIRE(lifecycle.prepare(acpi::transition_kind::reset, 0, 0, 0, true, ticket) ==
            acpi::transition_prepare_result::prepared);
    CHECK(ticket == ~u64{0});
    CHECK(lifecycle.cancel(ticket, 0));
    CHECK(lifecycle.prepare(acpi::transition_kind::reset, 0, 0, 0, true, ticket) ==
          acpi::transition_prepare_result::exhausted);
}

TEST_CASE("ACPI prepared transitions expire and stale tickets cannot be committed", "[acpi_fixed_hardware]")
{
    acpi::transition_lifecycle lifecycle;
    constexpr u64 start_ns = 100;
    u64 expired_ticket = 0;
    REQUIRE(lifecycle.prepare(acpi::transition_kind::sleep, 5, 5, start_ns, true, expired_ticket) ==
            acpi::transition_prepare_result::prepared);
    CHECK_FALSE(lifecycle.expire(start_ns + acpi::transition_lifecycle::ticket_lifetime_ns - 1));
    CHECK(lifecycle.active(start_ns + acpi::transition_lifecycle::ticket_lifetime_ns - 1));
    CHECK(lifecycle.expire(start_ns + acpi::transition_lifecycle::ticket_lifetime_ns));
    CHECK_FALSE(lifecycle.cancel(expired_ticket, start_ns + acpi::transition_lifecycle::ticket_lifetime_ns));

    u64 fresh_ticket = 0;
    REQUIRE(lifecycle.prepare(acpi::transition_kind::reset, 0, 0,
                              start_ns + acpi::transition_lifecycle::ticket_lifetime_ns, true,
                              fresh_ticket) == acpi::transition_prepare_result::prepared);
    u32 sleep_type_a = 0;
    u32 sleep_type_b = 0;
    CHECK_FALSE(lifecycle.begin_commit(acpi::transition_kind::sleep, expired_ticket,
                                       start_ns + acpi::transition_lifecycle::ticket_lifetime_ns, sleep_type_a,
                                       sleep_type_b));
    CHECK(lifecycle.begin_commit(acpi::transition_kind::reset, fresh_ticket,
                                 start_ns + acpi::transition_lifecycle::ticket_lifetime_ns, sleep_type_a,
                                 sleep_type_b));
}

TEST_CASE("ACPI transition timeout is disabled without a cross-CPU monotonic clock", "[acpi_fixed_hardware]")
{
    acpi::transition_lifecycle lifecycle;
    u64 ticket = 0;
    REQUIRE(lifecycle.prepare(acpi::transition_kind::reset, 0, 0, 100, false, ticket) ==
            acpi::transition_prepare_result::prepared);

    CHECK_FALSE(lifecycle.expire(~u64{0}));
    CHECK(lifecycle.active(~u64{0}));
    CHECK(lifecycle.cancel(ticket, ~u64{0}));
}
