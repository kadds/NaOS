#include <array>

#include <catch2/catch_test_macros.hpp>

#include "kernel/irq/vector_pool.hpp"
#include "kernel/pci/msi_message.hpp"

TEST_CASE("interrupt vector pool excludes architectural exception and APIC vectors", "[irq][pci][msi]")
{
    irq::vector_pool pool;

    for (u32 vector = 0; vector < 32; ++vector)
        CHECK(pool.unavailable(static_cast<u8>(vector)));
    CHECK(pool.unavailable(irq::vector_pool::local_apic_timer_vector));
    CHECK(pool.unavailable(irq::vector_pool::ipi_call_vector));
    CHECK(pool.unavailable(irq::vector_pool::ipi_reschedule_vector));
    CHECK(pool.unavailable(irq::vector_pool::ipi_tlb_vector));
    CHECK(pool.unavailable(irq::vector_pool::spurious_vector));
    CHECK(pool.unavailable(irq::vector_pool::interrupt_drain_vector));
}

TEST_CASE("MSI vectors outrank the fixed interrupt-drain barrier vector", "[irq][pci][msi]")
{
    irq::vector_pool pool;

    const u8 vector = pool.allocate_msi_vector();
    REQUIRE(vector != 0);
    CHECK(vector >= irq::vector_pool::first_msi_vector);
    CHECK((vector >> 4) > (irq::vector_pool::interrupt_drain_vector >> 4));
}

TEST_CASE("IOAPIC routes and MSI leases receive distinct vectors", "[irq][pci][msi]")
{
    irq::vector_pool pool;

    const u8 ioapic_vector = pool.allocate_ioapic_vector();
    const u8 msi_vector = pool.allocate_msi_vector();

    REQUIRE(ioapic_vector != 0);
    REQUIRE(msi_vector != 0);
    CHECK(ioapic_vector != msi_vector);
    CHECK(pool.unavailable(ioapic_vector));
    CHECK(pool.unavailable(msi_vector));
    CHECK_FALSE(pool.release_msi_vector(ioapic_vector));
    CHECK(pool.release_msi_vector(msi_vector));
    CHECK_FALSE(pool.unavailable(msi_vector));
    CHECK(pool.allocate_msi_vector() == msi_vector);
}

TEST_CASE("IOAPIC vector rollback only releases an IOAPIC-owned vector", "[irq][ioapic]")
{
    irq::vector_pool pool;

    const u8 ioapic_vector = pool.allocate_ioapic_vector();
    const u8 msi_vector = pool.allocate_msi_vector();
    REQUIRE(ioapic_vector != 0);
    REQUIRE(msi_vector != 0);

    CHECK_FALSE(pool.release_ioapic_vector(msi_vector));
    CHECK(pool.release_ioapic_vector(ioapic_vector));
    CHECK_FALSE(pool.unavailable(ioapic_vector));
    CHECK(pool.allocate_ioapic_vector() == ioapic_vector);
}

TEST_CASE("uncertain MSI routes quarantine their vectors instead of making them reusable", "[irq][pci][msi]")
{
    irq::vector_pool pool;

    const u8 quarantined_vector = pool.allocate_msi_vector();
    REQUIRE(quarantined_vector != 0);
    REQUIRE(pool.quarantine_msi_vector(quarantined_vector));

    CHECK(pool.unavailable(quarantined_vector));
    CHECK_FALSE(pool.release_msi_vector(quarantined_vector));
    CHECK(pool.allocate_msi_vector() != quarantined_vector);
}

TEST_CASE("vector exhaustion is bounded and all allocations are unique", "[irq][pci][msi]")
{
    irq::vector_pool pool;
    std::array<u8, irq::vector_pool::vector_count> allocated{};
    u32 count = 0;

    while (count < allocated.size())
    {
        const u8 vector = count % 2 == 0 ? pool.allocate_ioapic_vector() : pool.allocate_msi_vector();
        if (vector == 0)
            break;
        CHECK(pool.unavailable(vector));
        allocated[count++] = vector;
    }

    REQUIRE(count > 0);
    CHECK(count < allocated.size());
    CHECK(pool.allocate_ioapic_vector() == 0);
    CHECK(pool.allocate_msi_vector() == 0);

    for (u32 index = 0; index < count; ++index)
    {
        for (u32 other = index + 1; other < count; ++other)
            CHECK(allocated[index] != allocated[other]);
    }
}

TEST_CASE("kernel MSI message encoding bounds xAPIC destinations and vectors", "[irq][pci][msi]")
{
    pci::msi_message message{};

    REQUIRE(pci::detail::encode_xapic_msi_message(0x2a, 0x40, message));
    CHECK(message.address == 0xfee2a000);
    CHECK(message.data == 0x40);

    CHECK_FALSE(pci::detail::encode_xapic_msi_message(0x100, 0x40, message));
    CHECK_FALSE(pci::detail::encode_xapic_msi_message(0x2a, 0x1f, message));
    CHECK_FALSE(pci::detail::encode_xapic_msi_message(0x2a, irq::vector_pool::local_apic_timer_vector, message));
    CHECK_FALSE(pci::detail::encode_xapic_msi_message(0x2a, irq::vector_pool::ipi_call_vector, message));
    CHECK_FALSE(pci::detail::encode_xapic_msi_message(0x2a, irq::vector_pool::spurious_vector, message));
    CHECK(message.address == 0xfee2a000);
    CHECK(message.data == 0x40);

    REQUIRE(pci::detail::encode_xapic_msi_message(0xff, 0xfe, message));
    CHECK(message.address == 0xfeeff000);
    CHECK(message.data == 0xfe);
}
