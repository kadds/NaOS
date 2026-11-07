#include "kernel/arch/acpi/acpi.hpp"

#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/acpi/fixed_hardware.hpp"
#include "kernel/acpi/resource_authority.hpp"
#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/arch/acpi/facs_reservation.hpp"
#include "kernel/arch/acpi/facs_reservation_boot_test_hook.hpp"
#include "kernel/arch/acpi/physical_access.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/firmware_root.hpp"
#include "kernel/iommu/fault_dispatch.hpp"
#include "kernel/irq.hpp"
#include "kernel/kernel.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/pci/dma_domain.hpp"
#include "kernel/pci/dma_fault_source.hpp"
#include "kernel/pci/dma_quiesce.hpp"
#include "kernel/pci/msi_config.hpp"
#include "kernel/pci/msi_route.hpp"
#include "kernel/pci/msi_route_lifecycle.hpp"
#include "kernel/pci/msix_table_lease.hpp"
#include "kernel/pci/platform.hpp"
#include "kernel/pci/topology_lifecycle.hpp"
#include "kernel/physical_resource.hpp"
#include "kernel/smp.hpp"
#include "kernel/ucontext.hpp"
#include "pci_runtime.hpp"
#include <atomic>
#include <new>

KLOG_MODULE(acpi);

namespace arch::ACPI
{
namespace detail
{
acpi_boot::snapshot *firmware_snapshot = nullptr;
physical_resource::ledger *hardware_ledger = nullptr;
handle_t<physical_resource::root> *hardware_resource_root_object = nullptr;

physical_resource::root *hardware_resource_root_authority()
{
    if (hardware_resource_root_object == nullptr || !*hardware_resource_root_object)
        return nullptr;
    auto *authority = &**hardware_resource_root_object;
    return authority->resources() == hardware_ledger ? authority : nullptr;
}
} // namespace detail
namespace
{
using acpi_boot::table_record;
using detail::firmware_snapshot;
using detail::hardware_ledger;
using detail::hardware_resource_root_authority;
using detail::hardware_resource_root_object;

bool acpi_init = false;
handle_t<firmware::root> *firmware_root_object = nullptr;
handle_t<acpi::fixed_hardware> *fixed_hardware_object = nullptr;
handle_t<acpi::resource_authority> *resource_authority_object = nullptr;
acpi::fadt_fixed_inventory fadt_inventory_storage{};
void discard_partial_acpi_initialization(acpi_boot::snapshot *snapshot, physical_resource::ledger *ledger)
{
    pci_runtime::reset();
    if (resource_authority_object != nullptr)
    {
        memory::Delete(memory::KernelCommonAllocatorV, resource_authority_object);
        resource_authority_object = nullptr;
    }
    if (firmware_root_object != nullptr)
    {
        memory::Delete(memory::KernelCommonAllocatorV, firmware_root_object);
        firmware_root_object = nullptr;
    }
    if (fixed_hardware_object != nullptr)
    {
        memory::Delete(memory::KernelCommonAllocatorV, fixed_hardware_object);
        fixed_hardware_object = nullptr;
    }
    if (hardware_resource_root_object != nullptr)
    {
        memory::Delete(memory::KernelCommonAllocatorV, hardware_resource_root_object);
        hardware_resource_root_object = nullptr;
    }
    if (ledger != nullptr)
        memory::Delete(memory::KernelCommonAllocatorV, ledger);
    if (snapshot != nullptr)
        memory::Delete(memory::KernelCommonAllocatorV, snapshot);
    firmware_snapshot = nullptr;
    hardware_ledger = nullptr;
    fadt_inventory_storage = {};
    acpi_init = false;
}

struct acpi_initialization_transaction
{
    acpi_boot::snapshot *snapshot = nullptr;
    physical_resource::ledger *ledger = nullptr;
    bool committed = false;

    ~acpi_initialization_transaction()
    {
        if (!committed)
            discard_partial_acpi_initialization(snapshot, ledger);
    }
};

const u8 *map_physical(void *, u64 address, u64 length)
{
    if (length == 0 || address > ~u64(0) - length)
        return nullptr;
    return memory::pa2va<const u8 *>(phy_addr_t::from(address));
}

bool range_valid(void *, u64 address, u64 length)
{
    if (kernel_args == nullptr || kernel_args->mmap == 0 || length == 0 || address > ~u64(0) - length)
        return false;
    const auto *map = memory::pa2va<const kernel_memory_map_item *>(phy_addr_t::from(kernel_args->mmap));
    if (map == nullptr)
        return false;
    const u64 end = address + length;
    for (u64 index = 0; index < kernel_args->mmap_count; index++)
    {
        const u64 item_end = map[index].addr + map[index].len;
        if (item_end < map[index].addr)
            continue;
        if (address >= map[index].addr && end <= item_end && map[index].map_type != map_type_t::badram)
            return true;
    }
    return false;
}

bool device_range_valid(void *, u64 address, u64 length)
{
    if (kernel_args == nullptr || kernel_args->mmap == 0)
        return false;
    const auto *map = memory::pa2va<const kernel_memory_map_item *>(phy_addr_t::from(kernel_args->mmap));
    return acpi_boot::is_device_range_valid(*kernel_args, map, kernel_args->mmap_count, address, length);
}

bool iommu_register_range_valid(void *, u64 address, u64 length)
{
    if (kernel_args == nullptr || kernel_args->mmap == 0)
        return false;
    const auto *map = memory::pa2va<const kernel_memory_map_item *>(phy_addr_t::from(kernel_args->mmap));
    return acpi_boot::is_iommu_register_range_valid(*kernel_args, map, kernel_args->mmap_count, address, length);
}

const acpi_boot::physical_source &source()
{
    static const acpi_boot::physical_source value{map_physical, range_valid, device_range_valid,
                                                  iommu_register_range_valid, nullptr};
    return value;
}

bool make_boot_epoch(u64 &epoch)
{
    // A TSC/address-derived value is not a boot nonce: it can repeat across
    // resets and must not be used to scope externally cached opaque IDs.
    u64 entropy = 0;
    if (!arch::cpu_info::try_rdseed(entropy) && !arch::cpu_info::try_rdrand(entropy))
        return false;
    epoch = entropy ^ read_tsc_ordered() ^ reinterpret_cast<u64>(kernel_args) ^ reinterpret_cast<u64>(&acpi_init);
    if (epoch == 0)
        epoch = entropy == 0 ? ~u64(0) : entropy;
    return epoch != 0;
}

const table_record *find_table(u32 signature)
{
    if (firmware_snapshot == nullptr)
        return nullptr;
    for (const auto &table : firmware_snapshot->tables)
    {
        if (table.signature == signature)
            return &table;
    }
    return nullptr;
}

bool read_u16(const table_record &table, u64 offset, u16 &value)
{
    if (offset > table.length || sizeof(u16) > table.length - offset)
        return false;
    const auto *bytes = table.snapshot + offset;
    value = static_cast<u16>(bytes[0]) | static_cast<u16>(bytes[1]) << 8;
    return true;
}

bool read_u32(const table_record &table, u64 offset, u32 &value)
{
    if (offset > table.length || sizeof(u32) > table.length - offset)
        return false;
    const auto *bytes = table.snapshot + offset;
    value = static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 | static_cast<u32>(bytes[2]) << 16 |
            static_cast<u32>(bytes[3]) << 24;
    return true;
}

bool read_u64(const table_record &table, u64 offset, u64 &value)
{
    u32 low = 0;
    u32 high = 0;
    if (!read_u32(table, offset, low) || !read_u32(table, offset + sizeof(u32), high))
        return false;
    value = static_cast<u64>(low) | static_cast<u64>(high) << 32;
    return true;
}

bool subtable_bounds(const table_record &table, u64 &offset, u8 &type, u8 &length)
{
    if (offset > table.length || 2 > table.length - offset)
        return false;
    type = static_cast<u8>(table.snapshot[offset]);
    length = static_cast<u8>(table.snapshot[offset + 1]);
    if (length < 2 || length > table.length - offset)
        return false;
    offset += length;
    return true;
}

} // namespace

bool has_init() { return acpi_init; }

void init()
{
    if (acpi_init || kernel_args == nullptr)
        return;
    acpi_initialization_transaction transaction{};
    const u64 rsdp = kernel_args->rsdp != 0 ? kernel_args->rsdp : kernel_args->rsdp_old;
    if (rsdp == 0)
    {
        KLOG_WARN("ACPI: firmware root is absent");
        return;
    }

    auto *snapshot = memory::New<acpi_boot::snapshot>(memory::KernelCommonAllocatorV, memory::MemoryAllocatorV);
    if (snapshot == nullptr)
    {
        KLOG_WARN("ACPI: unable to allocate firmware snapshot");
        return;
    }
    transaction.snapshot = snapshot;
    u64 boot_epoch = 0;
    if (!make_boot_epoch(boot_epoch))
    {
        KLOG_WARN("ACPI: no hardware entropy source; refusing to publish a reusable boot epoch");
        return;
    }
    acpi_boot::error failure = acpi_boot::error::none;
    if (!acpi_boot::build_snapshot(source(), rsdp, boot_epoch, 1, *snapshot, failure))
    {
        KLOG_WARN("ACPI: rejected firmware tables ({})", static_cast<u32>(failure));
        return;
    }
    firmware_snapshot = snapshot;
    if (memory::KernelCommonAllocatorV == nullptr || memory::KernelVirtualAllocatorV == nullptr)
    {
        KLOG_WARN("ACPI: kernel allocators are unavailable for physical resource ledger");
        return;
    }
    hardware_ledger =
        memory::New<physical_resource::ledger>(memory::KernelCommonAllocatorV, memory::KernelVirtualAllocatorV);
    if (hardware_ledger == nullptr)
    {
        KLOG_WARN("ACPI: unable to allocate physical resource ledger");
        return;
    }
    transaction.ledger = hardware_ledger;
    hardware_resource_root_object = memory::New<handle_t<physical_resource::root>>(
        memory::KernelCommonAllocatorV, handle_t<physical_resource::root>::make(hardware_ledger));
    if (hardware_resource_root_object == nullptr || !*hardware_resource_root_object)
    {
        KLOG_WARN("ACPI: unable to allocate physical resource root");
        return;
    }
    auto *resource_root = hardware_resource_root_authority();
    if (resource_root == nullptr)
    {
        KLOG_WARN("ACPI: physical resource root is not bound to the ledger");
        return;
    }
    if (!pci_runtime::prepare_firmware_inventory(*snapshot, *resource_root))
    {
        KLOG_WARN("ACPI: unable to reserve PCI ECAM inventory");
        return;
    }
    const auto reserve_kernel_mmio = [&](u64 base, u64 length, physical_resource::kind kind) {
        if (base == 0 || length == 0 || base > ~u64(0) - length)
            return false;
        const u64 page_base = base & ~(memory::page_size - 1);
        const u64 end = base + length;
        if (end > ~u64(0) - (memory::page_size - 1))
            return false;
        const u64 page_end = (end + memory::page_size - 1) & ~(memory::page_size - 1);
        physical_resource::lease lease{};
        auto *authority = hardware_resource_root_authority();
        return authority != nullptr && authority->resources()->reserve(*authority, base, length, page_base, page_end,
                                                                       kind, physical_resource::cache::uncacheable,
                                                                       physical_resource::access::read_write,
                                                                       snapshot->firmware_generation, lease);
    };
    if (!reserve_kernel_mmio(reinterpret_cast<u64>(get_local_apic_base()()), memory::page_size,
                             physical_resource::kind::acpi_fixed_hardware))
    {
        KLOG_WARN("ACPI: unable to ledger local APIC fixed hardware");
        return;
    }
    for (const auto &[address, irq_base, enumeration_id] : get_io_apic_list())
    {
        (void)irq_base;
        (void)enumeration_id;
        if (!reserve_kernel_mmio(reinterpret_cast<u64>(address()), memory::page_size,
                                 physical_resource::kind::acpi_fixed_hardware))
        {
            KLOG_WARN("ACPI: unable to ledger I/O APIC fixed hardware");
            return;
        }
    }
    if (const auto hpet = get_hpet_base();
        hpet.has_value() && !reserve_kernel_mmio(reinterpret_cast<u64>(hpet.value()()), memory::page_size,
                                                 physical_resource::kind::acpi_fixed_hardware))
    {
        KLOG_WARN("ACPI: unable to ledger HPET fixed hardware");
        return;
    }
    const auto enumerate_facs_ranges = [&](auto reserve_range) {
        for (const auto &table : snapshot->tables)
        {
            if (table.signature == acpi_boot::signature_value("FACS") &&
                !reserve_range(table.physical_address, table.length))
            {
                KLOG_WARN("ACPI: unable to ledger FACS firmware state");
                return false;
            }
        }
        return true;
    };
    const auto initialize_after_facs_reservation = [&]() -> bool {
        // Reserve FADT-owned fixed hardware before publishing any user-facing
        // firmware capability. These are inventory entries only: no generic AML
        // region grant may consume them. I/O ranges use logical port overlap;
        // MMIO ranges retain page-envelope/cache conflict checks.
        const auto reserve_fixed_range = [&](u64 base, u64 length, physical_resource::kind kind) {
            if (base == 0 || length == 0 || base > ~u64(0) - length)
                return true;
            const u64 end = base + length;
            if (kind == physical_resource::kind::io_port && end > 0x1'0000)
                return false;
            if (kind != physical_resource::kind::io_port && !device_range_valid(nullptr, base, length))
                return false;
            if (end > ~u64(0) - (memory::page_size - 1))
                return false;
            const u64 page_base = base & ~(memory::page_size - 1);
            const u64 page_end = (end + memory::page_size - 1) & ~(memory::page_size - 1);
            physical_resource::lease lease{};
            const auto mode = kind == physical_resource::kind::io_port ? physical_resource::access::access
                                                                       : physical_resource::access::read_write;
            auto *authority = hardware_resource_root_authority();
            return authority != nullptr &&
                   authority->resources()->reserve(*authority, base, length, page_base, page_end, kind,
                                                   physical_resource::cache::uncacheable, mode,
                                                   snapshot->firmware_generation, lease);
        };
        bool has_fadt_inventory = false;
        if (const auto *fadt = find_table(acpi_boot::signature_value("FACP")); fadt != nullptr && fadt->length >= 94)
        {
            if (!acpi::parse_fadt_fixed_inventory(reinterpret_cast<const u8 *>(fadt->snapshot), fadt->length,
                                                  snapshot->boot_epoch, snapshot->firmware_generation,
                                                  fadt_inventory_storage))
            {
                KLOG_WARN("ACPI: invalid FADT fixed hardware inventory");
                return false;
            }
            if (!fadt_inventory_storage.active.hardware_reduced)
            {
                for (const auto &table : snapshot->tables)
                {
                    if (table.signature != acpi_boot::signature_value("FACS"))
                        continue;
                    constexpr u64 global_lock_offset = 16;
                    constexpr u64 global_lock_bytes = sizeof(u32);
                    if (table.length < global_lock_offset + global_lock_bytes ||
                        table.physical_address > ~u64{0} - global_lock_offset)
                    {
                        KLOG_WARN("ACPI: invalid FACS Global Lock extent");
                        return false;
                    }
                    fadt_inventory_storage.active.facs_global_lock_address =
                        table.physical_address + global_lock_offset;
                    break;
                }
            }
            for (u32 index = 0; index < fadt_inventory_storage.reserved_count; ++index)
            {
                const auto &range = fadt_inventory_storage.reserved[index];
                const auto kind =
                    range.space == 1 ? physical_resource::kind::io_port : physical_resource::kind::acpi_fixed_hardware;
                if (!reserve_fixed_range(range.address, range.length, kind))
                {
                    KLOG_WARN("ACPI: unable to ledger FADT fixed hardware at {}", log::hex(range.address));
                    return false;
                }
            }
            has_fadt_inventory = true;
        }
        if (has_fadt_inventory)
        {
            fixed_hardware_object = memory::New<handle_t<acpi::fixed_hardware>>(
                memory::KernelCommonAllocatorV, handle_t<acpi::fixed_hardware>::make(fadt_inventory_storage.active));
            if (fixed_hardware_object == nullptr || !*fixed_hardware_object)
            {
                KLOG_WARN("ACPI: unable to allocate fixed hardware capability");
                return false;
            }
        }

        // Allocate every capability object before enabling an IOMMU or
        // registering a provider. If any allocation fails, the initialization
        // transaction can discard the private snapshot and resource ledger
        // without leaving active hardware state behind.
        resource_authority_object = memory::New<handle_t<acpi::resource_authority>>(
            memory::KernelCommonAllocatorV,
            handle_t<acpi::resource_authority>::make(firmware_snapshot, hardware_resource_root_authority()));
        if (resource_authority_object == nullptr || !*resource_authority_object)
        {
            KLOG_WARN("ACPI: unable to allocate resource authority capability");
            return false;
        }
        firmware_root_object = memory::New<handle_t<firmware::root>>(memory::KernelCommonAllocatorV,
                                                                     handle_t<firmware::root>::make(firmware_snapshot));
        if (firmware_root_object == nullptr || !*firmware_root_object)
        {
            KLOG_WARN("ACPI: unable to allocate FirmwareRoot capability");
            return false;
        }
        if (!pci_runtime::create_discovery_fabric(*resource_root))
        {
            KLOG_WARN("ACPI: unable to allocate PciDiscoveryFabric capability");
            return false;
        }
#if defined(NAOS_TEST_ACPI_INIT_CAPABILITY_ROLLBACK)
        KLOG_INFO("ACPI TEST: capability publication rollback injected");
        return false;
#endif
        if (!pci::topology_lifetime().initialize(snapshot->boot_epoch, snapshot->firmware_generation,
                                                 snapshot->pci_fabric_generation))
        {
            KLOG_WARN("ACPI: unable to initialize PCI topology lifetime");
            return false;
        }

        u32 pci_functions_seen = 0;
        u32 pci_bus_masters_disabled = 0;
        pci::dma_quiesce::error pci_quiesce_failure = pci::dma_quiesce::error::invalid_region;
        const bool pci_dma_quiesced =
            !snapshot->mcfg_regions.empty() &&
            pci_runtime::quiesce_dma(*snapshot, pci_functions_seen, pci_bus_masters_disabled, pci_quiesce_failure);
        if (pci_dma_quiesced)
        {
            KLOG_INFO("ACPI: PCI DMA quiesced functions={} bus_masters_disabled={}", pci_functions_seen,
                      pci_bus_masters_disabled);
        }
        else
        {
            KLOG_WARN("ACPI: PCI DMA remains unquiesced error={} functions={} disabled={} mcfg_regions={}",
                      static_cast<u32>(pci_quiesce_failure), pci_functions_seen, pci_bus_masters_disabled,
                      snapshot->mcfg_regions.size());
        }

        if (!pci_runtime::initialize_providers(*snapshot, pci_dma_quiesced, *resource_root, device_range_valid,
                                               iommu_register_range_valid))
        {
            KLOG_WARN("ACPI: unable to install PCI platform operations");
            return false;
        }

        KLOG_INFO("ACPI: immutable firmware snapshot tables={} relations={} boot_epoch={} firmware_generation={}",
                  snapshot->tables.size(), snapshot->relations.size(), snapshot->boot_epoch,
                  snapshot->firmware_generation);
        return true;
    };

    if (!acpi_boot::apply_facs_reservation_boot_test_hook(*snapshot, *resource_root))
    {
        KLOG_WARN("ACPI: FACS reservation boot fixture failed");
        return;
    }
    if (!acpi_boot::reserve_facs_state_then(*resource_root, memory::page_size, snapshot->firmware_generation,
                                            enumerate_facs_ranges, initialize_after_facs_reservation))
    {
        return;
    }
    acpi_init = true;
    pci_runtime::activate();
    transaction.committed = true;
}

void enable()
{
    if (!acpi_init)
        return;
    pci_runtime::enable_fault_interrupts();
    // The kernel owns only the typed fixed-hardware event boundary. It does
    // not initialize ACPICA or evaluate AML. start_sci() may unmask SCI for
    // the FACS Global Lock handshake; other fixed/GPE events stay disabled
    // until platformd claims the published capability.
    if (fixed_hardware_object == nullptr || !*fixed_hardware_object)
        return;
    auto &fixed = **fixed_hardware_object;
    fixed.set_sci_gsi(arch::APIC::sci_gsi());
    if (!fixed.start_sci())
        KLOG_WARN("ACPI: unable to verify SCI masking or register its typed source; no user owner was published");
}

void start_deferred_services()
{
    if (acpi_init)
        pci_runtime::start_deferred_services();
}

freelibcxx::vector<io_apic_info> get_io_apic_list()
{
    freelibcxx::vector<io_apic_info> result(memory::MemoryAllocatorV);
    const auto *madt = find_table(acpi_boot::signature_value("APIC"));
    if (madt == nullptr || madt->length < 44)
        return result;
    u64 offset = 44;
    while (offset < madt->length)
    {
        const u64 start = offset;
        u8 type = 0;
        u8 length = 0;
        if (!subtable_bounds(*madt, offset, type, length))
            break;
        if (type == 1 && length >= 12)
        {
            u32 address = 0;
            u32 irq_base = 0;
            const u8 enumeration_id = static_cast<u8>(madt->snapshot[start + 2]);
            read_u32(*madt, start + 4, address);
            read_u32(*madt, start + 8, irq_base);
            result.push_back(io_apic_info{phy_addr_t::from(address), irq_base, enumeration_id});
        }
    }
    return result;
}

freelibcxx::vector<lapic_info> get_local_apic_list()
{
    freelibcxx::vector<lapic_info> result(memory::MemoryAllocatorV);
    const auto *madt = find_table(acpi_boot::signature_value("APIC"));
    if (madt == nullptr || madt->length < 44)
        return result;
    u64 offset = 44;
    while (offset < madt->length)
    {
        const u64 start = offset;
        u8 type = 0;
        u8 length = 0;
        if (!subtable_bounds(*madt, offset, type, length))
            break;
        lapic_info info{};
        if (type == 0 && length >= 8)
        {
            u32 flags = 0;
            info.processor_id = static_cast<u8>(madt->snapshot[start + 2]);
            info.apic_id = static_cast<u8>(madt->snapshot[start + 3]);
            read_u32(*madt, start + 4, flags);
            info.enabled = (flags & 1) != 0;
            result.push_back(info);
        }
        else if (type == 9 && length >= 16)
        {
            u32 flags = 0;
            u32 uid = 0;
            read_u32(*madt, start + 4, info.apic_id);
            read_u32(*madt, start + 8, flags);
            read_u32(*madt, start + 12, uid);
            info.processor_id = uid;
            info.enabled = (flags & 1) != 0;
            result.push_back(info);
        }
    }
    return result;
}

freelibcxx::vector<override_irq> get_override_irq_list()
{
    freelibcxx::vector<override_irq> result(memory::MemoryAllocatorV);
    const auto *madt = find_table(acpi_boot::signature_value("APIC"));
    if (madt == nullptr || madt->length < 44)
        return result;
    u64 offset = 44;
    while (offset < madt->length)
    {
        const u64 start = offset;
        u8 type = 0;
        u8 length = 0;
        if (!subtable_bounds(*madt, offset, type, length))
            break;
        if (type == 2 && length >= 10)
        {
            override_irq value{};
            value.bus = static_cast<u8>(madt->snapshot[start + 2]);
            value.irq_source = static_cast<u8>(madt->snapshot[start + 3]);
            read_u32(*madt, start + 4, value.gsi);
            read_u16(*madt, start + 8, value.flags);
            result.push_back(value);
        }
    }
    return result;
}

freelibcxx::optional<phy_addr_t> get_hpet_base()
{
    const auto *hpet = find_table(acpi_boot::signature_value("HPET"));
    u64 address = 0;
    if (hpet == nullptr || hpet->length < 52 || !read_u64(*hpet, 44, address) || address == 0)
        return freelibcxx::nullopt;
    return phy_addr_t::from(address);
}

freelibcxx::optional<pm_info> get_acpipm_base()
{
    const auto *fadt = find_table(acpi_boot::signature_value("FACP"));
    if (fadt == nullptr || fadt->length < 92)
        return freelibcxx::nullopt;
    u32 pm_timer = 0;
    u32 event_a = 0;
    if (!read_u32(*fadt, 76, pm_timer) || !read_u32(*fadt, 56, event_a) || static_cast<u8>(fadt->snapshot[91]) != 4)
        return freelibcxx::nullopt;

    pm_info result{};
    result.block_base = pm_timer;
    result.event_a = event_a;
    u32 flags = 0;
    read_u32(*fadt, 112, flags);
    result.bit32mode = (flags & (1u << 8)) != 0;
    result.xblock_base = nullptr;
    result.xevent_a = 0;
    result.use_io = true;
    if (fadt->length >= 220)
    {
        const u8 space_id = static_cast<u8>(fadt->snapshot[208]);
        u64 xblock = 0;
        u64 xevent = 0;
        read_u64(*fadt, 212, xblock);
        read_u64(*fadt, 152, xevent);
        if (xblock != 0)
        {
            result.xblock_base = reinterpret_cast<void *>(xblock);
            result.use_io = space_id == 1;
            result.xevent_a = xevent;
        }
    }
    return result;
}

bool is_8042_device_exists()
{
    const auto *fadt = find_table(acpi_boot::signature_value("FACP"));
    if (fadt == nullptr || fadt->length < 112)
        return true;
    u16 boot_flags = 0;
    if (!read_u16(*fadt, 109, boot_flags))
        return true;
    return (boot_flags & (1u << 1)) != 0;
}

phy_addr_t get_local_apic_base()
{
    const auto *madt = find_table(acpi_boot::signature_value("APIC"));
    if (madt == nullptr || madt->length < 44)
        return phy_addr_t::from(0xfee00000UL);
    const u64 physical_address_width = arch::cpu_info::get_feature(arch::cpu_info::feature::max_phy_addr);
    u64 base = 0;
    if (!acpi_boot::madt_local_apic_base(reinterpret_cast<const u8 *>(madt->snapshot), madt->length,
                                         physical_address_width, base))
        return phy_addr_t::from(0);
    return phy_addr_t::from(base);
}

freelibcxx::hash_map<u32, numa_info> get_numa_info()
{
    freelibcxx::hash_map<u32, numa_info> result(memory::MemoryAllocatorV);
    const auto *srat = find_table(acpi_boot::signature_value("SRAT"));
    if (srat == nullptr || srat->length < 48)
        return result;
    u64 offset = 48;
    while (offset < srat->length)
    {
        const u64 start = offset;
        u8 type = 0;
        u8 length = 0;
        if (!subtable_bounds(*srat, offset, type, length))
            break;
        numa_info info{};
        if (type == 0 && length >= 16)
        {
            info.domain = static_cast<u32>(static_cast<u8>(srat->snapshot[start + 2])) |
                          static_cast<u32>(static_cast<u8>(srat->snapshot[start + 9])) << 8 |
                          static_cast<u32>(static_cast<u8>(srat->snapshot[start + 10])) << 16 |
                          static_cast<u32>(static_cast<u8>(srat->snapshot[start + 11])) << 24;
            info.apic_id = static_cast<u8>(srat->snapshot[start + 3]);
            u32 flags = 0;
            read_u32(*srat, start + 4, flags);
            info.enabled = (flags & 1) != 0;
            result.insert(info.apic_id, info);
        }
        else if (type == 2 && length >= 16)
        {
            read_u32(*srat, start + 4, info.domain);
            read_u32(*srat, start + 8, info.apic_id);
            u32 flags = 0;
            read_u32(*srat, start + 12, flags);
            info.enabled = (flags & 1) != 0;
            result.insert(info.apic_id, info);
        }
    }
    return result;
}

handle_t<firmware::root> firmware_root()
{
    return !acpi_init || firmware_root_object == nullptr ? handle_t<firmware::root>() : *firmware_root_object;
}

handle_t<acpi::fixed_hardware> fixed_hardware()
{
    return !acpi_init || fixed_hardware_object == nullptr ? handle_t<acpi::fixed_hardware>() : *fixed_hardware_object;
}

handle_t<acpi::resource_authority> resource_authority()
{
    return !acpi_init || resource_authority_object == nullptr ? handle_t<acpi::resource_authority>()
                                                              : *resource_authority_object;
}

bool revoke_resource(u64 resource_id, u64 owner)
{
    return hardware_ledger != nullptr && hardware_ledger->revoke(resource_id, owner);
}

bool revoke_resource_kernel(u64 resource_id)
{
    return hardware_ledger != nullptr && hardware_ledger->revoke_kernel(resource_id);
}

bool unmap_resource_kernel(u64 resource_id)
{
    return hardware_ledger != nullptr && hardware_ledger->unmap_kernel(resource_id);
}

bool begin_map_resource_kernel(u64 resource_id, u64 base, u64 length)
{
    return hardware_ledger != nullptr && hardware_ledger->begin_mapping(resource_id, base, length);
}

bool end_map_resource_kernel(u64 resource_id)
{
    return hardware_ledger != nullptr && hardware_ledger->end_mapping(resource_id);
}

void on_process_exit(process_id owner)
{
    if (resource_authority_object != nullptr && *resource_authority_object &&
        (*resource_authority_object)->unbind_execution_owner(owner))
        (*resource_authority_object)->revoke_pci_config_scopes(owner);

    if (auto hardware = fixed_hardware())
        hardware->owner_exit(owner);
}

void on_thread_exit(process_id owner, u64 thread_id)
{
    if (auto hardware = fixed_hardware())
        hardware->release_global_lock_for_thread_exit(owner, thread_id);
}

bool revoke_owner_resources(process_id owner)
{
    if (resource_authority_object != nullptr && *resource_authority_object)
    {
        (*resource_authority_object)->revoke_pci_config_scopes(owner);
        (*resource_authority_object)->unbind_execution_owner(owner);
    }
    return hardware_ledger != nullptr && hardware_ledger->revoke_owner(owner);
}

bool revoke_thread_resources(process_id owner, u64 thread_id)
{
    if (resource_authority_object != nullptr && *resource_authority_object)
        (*resource_authority_object)->unbind_execution_thread(owner, thread_id);
    return hardware_ledger != nullptr && hardware_ledger->revoke_thread(owner, thread_id);
}

bool is_device_range(u64 address, u64 length) { return device_range_valid(nullptr, address, length); }

} // namespace arch::ACPI
