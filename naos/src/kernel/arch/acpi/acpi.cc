#include "kernel/arch/acpi/acpi.hpp"
#include "common.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"
#include "kernel/types.hpp"
ExportC
{
#include "acexcep.h"
#include "acpi.h"
#include "acpixf.h"
#include "actbl.h"
#include "actbl1.h"
#include "actbl2.h"
#include "actbl3.h"
#include "actypes.h"
}
#include "efi/efi.h"

namespace arch::ACPI
{

bool acpi_init = false;

bool has_init() { return acpi_init; }

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
        for (unsigned int i = 0; i < p->NumberOfTableEntries; i++)
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
    acpi_init = true;
}

UINT32 on_power_button(void *context)
{
    trace::info("ACPI: power down");
    return ACPI_INTERRUPT_HANDLED;
}

void enable()
{
    if (acpi_init)
    {
        AcpiEnableSubsystem(0);
        AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, on_power_button, 0);
    }
}

UINT32 on_acpi_timer(void *context)
{
    auto ctx = reinterpret_cast<freelibcxx::tuple<timer_fn, u64> *>(context);

    const auto [f, user_data] = *ctx;
    auto ret = f(user_data);
    if (ret == irq::request_result::no_handled)
    {
        return ACPI_INTERRUPT_NOT_HANDLED;
    }
    return ACPI_INTERRUPT_HANDLED;
}

bool register_timer(timer_fn fn, u64 user_data)
{
    auto context =
        memory::MemoryAllocatorV->New<freelibcxx::tuple<timer_fn, u64>>(freelibcxx::make_tuple(fn, user_data));

    if (ACPI_FAILURE(AcpiInstallFixedEventHandler(ACPI_EVENT_PMTIMER, on_acpi_timer, (void *)context)))
    {
        return false;
    }
    return true;
}

void unregister_timer() { AcpiRemoveFixedEventHandler(ACPI_EVENT_PMTIMER, on_acpi_timer); }

template <typename TABLE> TABLE *load_table(const char *SIG, bool required = true)
{
    if (!acpi_init)
    {
        return nullptr;
    }
    ACPI_TABLE_HEADER *table = nullptr;
    char sig[5];
    memcpy(sig, SIG, sizeof(sig));
    auto status = AcpiGetTable(sig, 0, &table);
    if (ACPI_FAILURE(status) && required)
    {
        trace::panic("get ", SIG, " table fail status ", status);
    }
    return reinterpret_cast<TABLE *>(table);
}

freelibcxx::vector<freelibcxx::tuple<phy_addr_t, u32>> get_io_apic_list()
{
    auto madt = load_table<ACPI_TABLE_MADT>(ACPI_SIG_MADT);
    ACPI_SUBTABLE_HEADER *header = reinterpret_cast<ACPI_SUBTABLE_HEADER *>((char *)madt + sizeof(*madt));
    freelibcxx::vector<freelibcxx::tuple<phy_addr_t, u32>> ret(memory::MemoryAllocatorV);
    while ((char *)header < (char *)madt + madt->Header.Length)
    {
        if (header->Type == ACPI_MADT_TYPE_IO_APIC)
        {
            ACPI_MADT_IO_APIC *io_apic = reinterpret_cast<ACPI_MADT_IO_APIC *>(header);
            ret.push_back(phy_addr_t::from(io_apic->Address), io_apic->GlobalIrqBase);
        }

        header = reinterpret_cast<ACPI_SUBTABLE_HEADER *>((char *)header + header->Length);
    }
    return ret;
}

freelibcxx::vector<override_irq> get_override_irq_list()
{
    auto madt = load_table<ACPI_TABLE_MADT>(ACPI_SIG_MADT);
    ACPI_SUBTABLE_HEADER *header = reinterpret_cast<ACPI_SUBTABLE_HEADER *>((char *)madt + sizeof(*madt));
    freelibcxx::vector<override_irq> ret(memory::MemoryAllocatorV);
    while ((char *)header < (char *)madt + madt->Header.Length)
    {
        if (header->Type == ACPI_MADT_TYPE_INTERRUPT_OVERRIDE)
        {
            ACPI_MADT_INTERRUPT_OVERRIDE *io_apic = reinterpret_cast<ACPI_MADT_INTERRUPT_OVERRIDE *>(header);
            override_irq irq;
            irq.bus = io_apic->Bus;
            irq.gsi = io_apic->GlobalIrq;
            irq.irq_source = io_apic->SourceIrq;
            irq.flags = io_apic->IntiFlags;
            ret.push_back(irq);
        }
        header = reinterpret_cast<ACPI_SUBTABLE_HEADER *>((char *)header + header->Length);
    }
    return ret;
}

freelibcxx::optional<phy_addr_t> get_hpet_base()
{
    auto hpet = load_table<ACPI_TABLE_HPET>(ACPI_SIG_HPET, false);
    if (hpet != nullptr)
    {
        return phy_addr_t::from(hpet->Address.Address);
    }
    return freelibcxx::nullopt;
}

freelibcxx::optional<pm_info> get_acpipm_base()
{
    auto fadt = load_table<ACPI_TABLE_FADT>(ACPI_SIG_FADT, false);
    if (fadt != nullptr)
    {
        if (fadt->PmTimerLength != 4)
        {
            return freelibcxx::nullopt;
        }
        pm_info info;
        info.bit32mode = fadt->Flags & ACPI_FADT_32BIT_TIMER;
        info.block_base = fadt->PmTimerBlock;
        info.event_a = fadt->Pm1aEventBlock;
        if (fadt->Header.Length < offsetof(ACPI_TABLE_FADT, XPmTimerBlock))
        {
            info.xblock_base = 0;
            info.use_io = false;
        }
        else
        {
            info.xblock_base = (void *)fadt->XPmTimerBlock.Address;
            info.use_io = fadt->XPmTimerBlock.SpaceId == 1;
            info.xevent_a = fadt->XPm1aEventBlock.Address;
        }
        return info;
    }
    return freelibcxx::nullopt;
}

phy_addr_t get_local_apic_base()
{
    auto madt = load_table<ACPI_TABLE_MADT>(ACPI_SIG_MADT);
    return phy_addr_t::from(madt->Address);
}

} // namespace arch::ACPI