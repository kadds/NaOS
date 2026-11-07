#include <catch2/catch_test_macros.hpp>

#include "kernel/iommu/intel_vtd_interrupt.hpp"
#include "kernel/pci/msi_message.hpp"

TEST_CASE("VT-d PCI MSI IRTE validates the exact requester ID and xAPIC target", "[intel_vtd][msi]")
{
    iommu::intel_vtd::interrupt_remapping_entry entry{};

    REQUIRE(iommu::intel_vtd::encode_pci_msi_irte(0xabcd, 0x2a, 0x45, false, entry));
    CHECK(entry.low == (1ull | (0x45ull << 16) | (0x2aull << 40)));
    CHECK(entry.high == ((0x1ull << 18) | 0xabcdull));
}

TEST_CASE("VT-d IOAPIC IRTE preserves level trigger mode and the exact source ID", "[intel_vtd][ir]")
{
    iommu::intel_vtd::interrupt_remapping_entry entry{};

    REQUIRE(iommu::intel_vtd::encode_ioapic_irte(0x9abc, 0x2a, 0x45, true, false, entry));
    CHECK(entry.low == (1ull | (1ull << 4) | (0x45ull << 16) | (0x2aull << 40)));
    CHECK(entry.high == ((1ull << 18) | 0x9abcull));

    const auto previous = entry;
    CHECK_FALSE(iommu::intel_vtd::encode_ioapic_irte(0x9abc, 0x100, 0x45, true, false, entry));
    CHECK(entry.low == previous.low);
    CHECK(entry.high == previous.high);
}

TEST_CASE("VT-d IOAPIC remappable RTE encodes index, format, polarity, trigger and mask", "[intel_vtd][ir]")
{
    u64 rte = 0;

    REQUIRE(iommu::intel_vtd::encode_ioapic_remappable_rte(0x8123, 0x45, true, true, true, rte));
    CHECK(rte ==
          (0x45ull | (1ull << 11) | (1ull << 13) | (1ull << 15) | (1ull << 16) | (1ull << 48) | (0x0123ull << 49)));

    const u64 previous = rte;
    CHECK_FALSE(iommu::intel_vtd::encode_ioapic_remappable_rte(0x0123, 0x1f, false, false, false, rte));
    CHECK(rte == previous);
}

TEST_CASE("PCI MSI remappable message uses the prepared IRTE index and no naked vector", "[intel_vtd][msi]")
{
    pci::msi_message message{};

    REQUIRE(iommu::intel_vtd::encode_remappable_msi_message(0x00a5, message));
    CHECK(message.address == (0xfee00000ull | (0x00a5ull << 5) | (1ull << 4) | (1ull << 3)));
    CHECK(message.data == 0);

    const auto previous = message;
    CHECK_FALSE(
        iommu::intel_vtd::encode_remappable_msi_message(iommu::intel_vtd::interrupt_remapping_entry_count, message));
    CHECK(message.address == previous.address);
    CHECK(message.data == previous.data);
}

TEST_CASE("VT-d PCI MSI IRTE permits a full x2APIC target only in EIME mode", "[intel_vtd][msi]")
{
    iommu::intel_vtd::interrupt_remapping_entry entry{};

    REQUIRE(iommu::intel_vtd::encode_pci_msi_irte(0x0123, 0x12345678, 0x46, true, entry));
    CHECK(entry.low == (1ull | (0x46ull << 16) | (0x12345678ull << 32)));
    CHECK(entry.high == ((0x1ull << 18) | 0x0123ull));

    const auto previous = entry;
    CHECK_FALSE(iommu::intel_vtd::encode_pci_msi_irte(0x0123, 0x100, 0x46, false, entry));
    CHECK(entry.low == previous.low);
    CHECK(entry.high == previous.high);
}

TEST_CASE("VT-d PCI MSI IRTE rejects vectors owned by exceptions and the local APIC", "[intel_vtd][msi]")
{
    iommu::intel_vtd::interrupt_remapping_entry entry{0x1234, 0x5678};
    const auto previous = entry;

    CHECK_FALSE(iommu::intel_vtd::encode_pci_msi_irte(0x0001, 0x2a, 0x1f, false, entry));
    CHECK_FALSE(iommu::intel_vtd::encode_pci_msi_irte(0x0001, 0x2a, 128, false, entry));
    CHECK_FALSE(iommu::intel_vtd::encode_pci_msi_irte(0x0001, 0x2a, 250, false, entry));
    CHECK_FALSE(iommu::intel_vtd::encode_pci_msi_irte(0x0001, 0x2a, 251, false, entry));
    CHECK_FALSE(iommu::intel_vtd::encode_pci_msi_irte(0x0001, 0x2a, 252, false, entry));
    CHECK_FALSE(iommu::intel_vtd::encode_pci_msi_irte(0x0001, 0x2a, 255, false, entry));

    CHECK(entry.low == previous.low);
    CHECK(entry.high == previous.high);
}

TEST_CASE("VT-d fault events use a direct xAPIC message and reject reserved routes", "[intel_vtd][fault][irq]")
{
    iommu::intel_vtd::fault_event_message message{};

    REQUIRE(iommu::intel_vtd::encode_xapic_fault_event_message(0x2a, 0x55, message));
    CHECK(message.address == (0xfee00000u | (0x2au << 12)));
    CHECK(message.data == 0x55);
    CHECK(message.upper_address == 0);

    const auto previous = message;
    CHECK_FALSE(iommu::intel_vtd::encode_xapic_fault_event_message(0x100, 0x55, message));
    CHECK_FALSE(
        iommu::intel_vtd::encode_xapic_fault_event_message(0x2a, irq::vector_pool::interrupt_drain_vector, message));
    CHECK_FALSE(iommu::intel_vtd::encode_xapic_fault_event_message(0x2a, 0x20, message));
    CHECK(message.address == previous.address);
    CHECK(message.data == previous.data);
    CHECK(message.upper_address == previous.upper_address);
}
