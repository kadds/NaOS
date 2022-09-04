#include "kernel/arch/acpi/acpi.hpp"
#include "common.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/trace.hpp"
ExportC
{
#include "acexcep.h"
#include "acpi.h"
#include "acpixf.h"
#include "actbl.h"
#include "actbl1.h"
#include "actbl2.h"
#include "actbl3.h"
}
#include "efi/efi.h"

namespace arch::ACPI
{

void init()
{
    auto p = (EFI_SYSTEM_TABLE *)kernel_args->efi_system_table;
    if (p != nullptr)
    {
        p = (EFI_SYSTEM_TABLE *)memory::pa2va(phy_addr_t::from(p));
        EFI_GUID target = ACPI_TABLE_GUID;
        EFI_GUID target2 = ACPI_20_TABLE_GUID;
        EFI_CONFIGURATION_TABLE *cfg_table =
            (EFI_CONFIGURATION_TABLE *)memory::pa2va(phy_addr_t::from(p->ConfigurationTable));
        for (int i = 0; i < p->NumberOfTableEntries; i++)
        {
            if (memcmp(&cfg_table[i].VendorGuid, &target, sizeof(target)) == 0)
            {
                kernel_args->rsdp_old = (u64)cfg_table[i].VendorTable;
            }
            else if (memcmp(&cfg_table[i].VendorGuid, &target2, sizeof(target2)) == 0)
            {
                kernel_args->rsdp = (u64)cfg_table[i].VendorTable;
            }
        }
    }

    trace::debug("rsdp ", trace::hex(kernel_args->rsdp), " old rsdp ", trace::hex(kernel_args->rsdp_old));
    if (kernel_args->rsdp == 0 && kernel_args->rsdp_old == 0)
    {
        trace::warning("rsdp empty");
    }
    ACPI_PHYSICAL_ADDRESS addr = 0;
    AcpiFindRootPointer(&addr);
    trace::info("find rsdp ", trace::hex(addr));
    if (addr != kernel_args->rsdp_old && addr != 0)
    {
        kernel_args->rsdp = addr;
    }

    AcpiInitializeSubsystem();
    AcpiInitializeTables(nullptr, 0, 0);
    AcpiEnableSubsystem(0);

    ACPI_MADT_IO_APIC ioapic;
    ACPI_TABLE_HEADER *table = nullptr;
    auto status = AcpiGetTable(ACPI_SIG_MADT, 0, &table);
    if (ACPI_FAILURE(status))
    {
        trace::panic("get mdat table fail status ", status);
    }
}

ACPI_TABLE_MADT *load_madt()
{
    ACPI_TABLE_HEADER *table = nullptr;
    auto status = AcpiGetTable(ACPI_SIG_MADT, 0, &table);
    if (ACPI_FAILURE(status))
    {
        trace::panic("get mdat table fail status ", status);
    }
    ACPI_TABLE_MADT *madt_table = (ACPI_TABLE_MADT *)table;
    return madt_table;
}
ACPI_TABLE_MCFG *load_mcfg()
{
    ACPI_TABLE_HEADER *table = nullptr;
    auto status = AcpiGetTable(ACPI_SIG_MCFG, 0, &table);
    if (ACPI_FAILURE(status))
    {
        trace::panic("get mcfg table fail status ", status);
    }
    ACPI_TABLE_MCFG *mcfg_table = (ACPI_TABLE_MCFG *)table;
    return mcfg_table;
}

phy_addr_t get_io_apic_base()
{
    auto madt = load_madt();
    auto len = madt->Header.Length - sizeof(*madt);
    ACPI_SUBTABLE_HEADER *header = reinterpret_cast<ACPI_SUBTABLE_HEADER *>((char *)madt + sizeof(*madt));
    while (true)
    {
        if (header->Type == ACPI_MADT_TYPE_LOCAL_APIC)
        {
        }
        if (header->Type == ACPI_MADT_TYPE_IO_APIC)
        {
            ACPI_MADT_IO_APIC *io_apic = reinterpret_cast<ACPI_MADT_IO_APIC *>(header);
            return phy_addr_t::from(io_apic->Address);
        }

        if (len < header->Length)
        {
            break;
        }
        len -= header->Length;
        header = reinterpret_cast<ACPI_SUBTABLE_HEADER *>((char *)header + header->Length);
    }
    trace::panic("io apic address not found");
}

phy_addr_t get_local_apic_base() { return phy_addr_t::from(load_madt()->Address); }

} // namespace arch::ACPI