#include "kernel/arch/io_apic.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/common.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>
KLOG_MODULE(arch);
namespace arch::APIC
{
class io_map_t;
namespace
{
constexpr u32 max_supported_gsi = 0xff;
constexpr u32 max_remote_irr_polls = 1'000'000;
lock::spinlock_t io_apic_access_lock;
bool io_interrupt_remapping_transition = false;
std::atomic<io_map_t *> io_eoi_owner[irq::vector_pool::vector_count]{};
std::atomic<u8> io_eoi_index[irq::vector_pool::vector_count]{};

void register_io_eoi_route(u8 vector, io_map_t *owner, u8 index)
{
    io_eoi_index[vector].store(index, std::memory_order_relaxed);
    io_eoi_owner[vector].store(owner, std::memory_order_release);
}
} // namespace

class io_map_t
{
  public:
    io_map_t(phy_addr_t base, int irq_base, u8 expected_id)
        : base_phy_(base)
        , irq_base_(irq_base)
        , expected_id_(expected_id)
        , io_entry_(memory::MemoryAllocatorV)
        , vectors_(memory::MemoryAllocatorV)
        , ir_was_enabled_(memory::MemoryAllocatorV)
    {
    }

    void update_base_addr(void *base)
    {
        index_addr_ = base;
        data_addr_ = (char *)index_addr_ + 0x10;
        EOI_addr_ = (char *)index_addr_ + 0x40;
    }

    void update_info(u8 count, u8 id, u8 version)
    {
        rte_count_ = count;
        id_ = id;
        version_ = version;
        io_entry_.resize(count, {});
        vectors_.resize(count, 0);
        ir_was_enabled_.resize(count, 0);

        // disable all
        for (u8 i = 0; i < count; i++)
        {
            io_write_register(0x10 + i * 2, 0x10020 + i);
        }
    }

    bool configure_id()
    {
        if (expected_id_ > 0x0f)
        {
            identity_valid_ = false;
            return false;
        }

        const u32 current = io_read_register_32(0);
        constexpr u32 io_apic_id_mask = 0x0fU << 24;
        const u32 desired = (current & ~io_apic_id_mask) | (static_cast<u32>(expected_id_) << 24);
        if (desired != current)
            io_write_register_32(0, desired);
        id_ = static_cast<u8>((io_read_register_32(0) >> 24) & 0x0f);
        identity_valid_ = id_ == expected_id_;
        return identity_valid_;
    }

    bool identity_valid() const { return identity_valid_; }
    u8 id() const { return id_; }

    u32 io_read_register_32(u16 index)
    {
        u32 rt;
        *(volatile u16 *)index_addr_ = index;
        _mfence();
        rt = *(volatile u32 *)data_addr_;
        _mfence();
        return rt;
    }
    void io_write_register_32(u16 index, u32 v)
    {
        *(volatile u16 *)index_addr_ = index;
        _mfence();
        *(volatile u32 *)data_addr_ = v;
        _mfence();
    }
    u64 io_read_register(u16 index)
    {
        u64 rt;
        rt = io_read_register_32(index + 1);
        rt <<= 32;
        rt |= io_read_register_32(index);
        return rt;
    }

    void io_write_register(u16 index, u64 value)
    {
        io_write_register_32(index, value);
        value >>= 32;
        io_write_register_32(index + 1, value);
    }

    phy_addr_t base_address() const { return base_phy_; }

    u8 irq_setup(u8 index, u8 vector, io_entry *entry)
    {
        if (!identity_valid_ || index >= io_entry_.size() || vector == 0 || entry == nullptr)
            return 0;
        if (entry->is_remapped)
        {
            // The selected provider supplies a complete, masked IOAPIC RTE.
            if (!entry->is_disable || entry->delivery_mode != io_entry::mode_t::fixed ||
                !entry->interrupt_route.valid() || entry->remapped_redirection_entry == 0 ||
                (entry->remapped_redirection_entry & (1ull << 16)) == 0)
                return 0;

            io_entry_[index] = *entry;
            vectors_[index] = vector;
            io_write_register(0x10 + index * 0x2, entry->remapped_redirection_entry);
            register_io_eoi_route(vector, this, index);
            return vector;
        }

        u32 flags = 0;
        if (entry->is_disable)
            flags |= 1U << 16;
        if (entry->is_level_trigger_mode)
            flags |= 1U << 15;
        if (entry->is_logic_mode)
            flags |= 1U << 11;
        if (entry->low_level_polarity)
            flags |= 1U << 13;

        flags |= ((u16)entry->delivery_mode) << 8;

        io_entry_[index] = *entry;
        vectors_[index] = vector;
        u64 data = vector | flags | (((u64)entry->dest_apic_id) << 56);
        io_write_register(0x10 + index * 0x2, data);
        register_io_eoi_route(vector, this, index);

        return vector;
    }

    u8 route_vector(u8 index) { return index < vectors_.size() ? vectors_[index] : 0; }

    u8 irq_base() const { return irq_base_; }
    u8 rte_count() const { return rte_count_; }

    bool enable(u8 index)
    {
        if (!identity_valid_ || index >= io_entry_.size())
            return false;
        if (arch::ACPI::interrupt_remapping_activation_uncertain())
            return false;
        const bool remapping_enabled = arch::ACPI::interrupt_remapping_enabled();
        if (remapping_enabled != io_entry_[index].is_remapped)
            return false;
        const u16 register_index = static_cast<u16>(0x10 + index * 0x2);
        io_write_register(register_index, io_read_register(register_index) & ~(1UL << 16));
        const bool enabled = (io_read_register(register_index) & (1UL << 16)) == 0;
        if (enabled)
            io_entry_[index].is_disable = false;
        return enabled;
    }

    bool disable(u8 index)
    {
        if (index >= io_entry_.size())
            return false;
        const u16 register_index = static_cast<u16>(0x10 + index * 2);
        io_write_register(register_index, io_read_register(register_index) | (1UL << 16));
        const bool disabled = (io_read_register(register_index) & (1UL << 16)) != 0;
        if (disabled)
            io_entry_[index].is_disable = true;
        return disabled;
    }

    bool mask_configured_routes_for_remapping()
    {
        for (u32 index = 0; index < io_entry_.size(); ++index)
        {
            ir_was_enabled_[index] = 0;
            if (vectors_[index] == 0)
                continue;
            const u16 register_index = static_cast<u16>(0x10 + index * 2);
            const bool was_enabled = (io_read_register(register_index) & (1ull << 16)) == 0;
            ir_was_enabled_[index] = was_enabled ? 1 : 0;
            if (was_enabled && !disable(static_cast<u8>(index)))
                return false;
        }
        return true;
    }

    bool remap_configured_routes()
    {
        const u8 bsp_apic_id = static_cast<u8>(arch::APIC::local_ID());
        for (u32 index = 0; index < io_entry_.size(); ++index)
        {
            if (vectors_[index] == 0 || io_entry_[index].is_remapped)
                continue;

            io_entry candidate = io_entry_[index];
            candidate.is_disable = true;
            if (candidate.dest_apic_id != bsp_apic_id || candidate.delivery_mode != io_entry::mode_t::fixed ||
                candidate.is_logic_mode)
            {
                if (ir_was_enabled_[index] != 0)
                    return false;
                continue;
            }
            if (candidate.is_level_trigger_mode && !wait_remote_irr_clear(static_cast<u8>(index)))
            {
                if (ir_was_enabled_[index] != 0)
                    return false;
                continue;
            }

            iommu::ioapic_route_encoding route{};
            if (!arch::ACPI::install_ioapic_interrupt_route(id_, candidate.dest_apic_id, vectors_[index],
                                                            candidate.low_level_polarity,
                                                            candidate.is_level_trigger_mode, route))
            {
                if (ir_was_enabled_[index] != 0)
                    return false;
                continue;
            }

            candidate.is_remapped = true;
            candidate.interrupt_route = route.token;
            candidate.remapped_redirection_entry = route.redirection_entry;
            u8 configured = 0;
            {
                uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
                if (io_interrupt_remapping_transition)
                    configured = irq_setup(static_cast<u8>(index), vectors_[index], &candidate);
            }
            if (configured == 0)
            {
                (void)arch::ACPI::disable_ioapic_interrupt_route(route.token);
                if (ir_was_enabled_[index] != 0)
                    return false;
            }
        }
        return true;
    }

    bool wait_remote_irr_clear(u8 index)
    {
        const u16 register_index = static_cast<u16>(0x10 + index * 2);
        for (u32 attempt = 0; attempt < max_remote_irr_polls; ++attempt)
        {
            u64 rte = 0;
            {
                uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
                rte = io_read_register(register_index);
            }
            if ((rte & (1ull << 14)) == 0)
                return true;
            __asm__ __volatile__("pause" : : : "memory");
        }
        return false;
    }

    bool restore_legacy_routes()
    {
        if (arch::ACPI::interrupt_remapping_activation_uncertain())
            return false;

        // The RTEs were masked before route installation. Retire every
        // provider-owned route before replacing those RTEs with legacy
        // destinations, and do not hold io_apic_access_lock while waiting for
        // IOMMU commands to complete.
        for (u32 index = 0; index < io_entry_.size(); ++index)
        {
            if (vectors_[index] == 0 || !io_entry_[index].is_remapped)
                continue;
            if (!arch::ACPI::disable_ioapic_interrupt_route(io_entry_[index].interrupt_route))
                return false;

            // Retire one route at a time. If a later retirement fails, this
            // RTE has already been converted to a masked legacy route, while
            // every remaining provider route is still masked in hardware.
            uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
            io_entry legacy_entry = io_entry_[index];
            legacy_entry.is_remapped = false;
            legacy_entry.interrupt_route = {};
            legacy_entry.remapped_redirection_entry = 0;
            legacy_entry.is_disable = true;
            if (irq_setup(static_cast<u8>(index), vectors_[index], &legacy_entry) == 0)
                return false;
        }

        for (u32 index = 0; index < io_entry_.size(); ++index)
        {
            if (ir_was_enabled_[index] == 0)
                continue;
            if (arch::ACPI::interrupt_remapping_activation_uncertain() || !enable(static_cast<u8>(index)))
            {
                uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
                for (u32 route_index = 0; route_index < io_entry_.size(); ++route_index)
                    if (vectors_[route_index] != 0)
                        io_entry_[route_index].is_disable = disable(static_cast<u8>(route_index));
                return false;
            }
            ir_was_enabled_[index] = 0;
        }
        return true;
    }

    void restore_enabled_remapped_routes()
    {
        for (u32 index = 0; index < io_entry_.size(); ++index)
        {
            if (ir_was_enabled_[index] == 0)
                continue;
            if (!io_entry_[index].is_remapped || !enable(static_cast<u8>(index)))
                KLOG_ERROR("IO APIC {} route {} remains masked after interrupt-remapping enable", id_, index);
            ir_was_enabled_[index] = 0;
        }
    }

    bool remapping_is_ready_to_activate()
    {
        for (u32 index = 0; index < io_entry_.size(); ++index)
        {
            const u64 rte = io_read_register(static_cast<u16>(0x10 + index * 2));
            if ((rte & (1ull << 16)) == 0 && !io_entry_[index].is_remapped)
                return false;
        }
        return true;
    }

    void eoi(u8 index)
    {
        kassert(index < io_entry_.size(), "index check fail");
        if (io_entry_[index].is_level_trigger_mode && vectors_[index] != 0)
            *(u32 *)EOI_addr_ = vectors_[index];
    }

  private:
    phy_addr_t base_phy_;
    void *index_addr_;
    void *data_addr_;
    void *EOI_addr_;
    u8 irq_base_;
    u8 rte_count_;
    u8 id_;
    u8 expected_id_;
    u8 version_;
    bool identity_valid_ = false;
    freelibcxx::vector<io_entry> io_entry_;
    freelibcxx::vector<u8> vectors_;
    freelibcxx::vector<u8> ir_was_enabled_;
};

freelibcxx::vector<io_map_t> *io_map_vec;
u32 pit_gsi = 0;
u32 sci_gsi_number = 9;
u8 sci_interrupt_vector = 0;

void io_init()
{
    KLOG_DEBUG("IO APIC init");
    bool acpi = cmdline::get_bool("acpi", false);
    io_map_vec = memory::MemoryAllocatorV->New<freelibcxx::vector<io_map_t>>(memory::MemoryAllocatorV);

    if (acpi)
    {
        for (const auto &info : arch::ACPI::get_io_apic_list())
        {
            io_map_t io_map(info.address, info.gsi_base, info.enumeration_id);
            io_map_vec->push_back(io_map);
        }
    }
    else
    {
        io_map_vec->push_back(phy_addr_t::from((void *)0xFEC0'0000UL), 0, 15);
    }

    int i = 0;

    for (auto &io_map : *io_map_vec)
    {
        u64 map_base = memory::alloc_io_mmap_address(paging::frame_size::size_2mb, paging::frame_size::size_2mb);
        auto &paging = memory::kernel_vm_info->paging();
        paging.big_page_map_to(reinterpret_cast<void *>(map_base), paging::big_pages, io_map.base_address(),
                               paging::flags::writable | paging::flags::cache_disable | paging::flags::write_through,
                               0);

        paging.reload();
        io_map.update_base_addr(reinterpret_cast<void *>(map_base));

        // Keep the hardware ID consistent with the MADT value. Interrupt
        // remapping identifies this source through the DMAR IOAPIC scope.
        const bool id_valid = io_map.configure_id();
        u8 rte = (io_map.io_read_register_32(0x1) >> 16) & 0xFF;
        u8 id = io_map.id();
        u8 version = (io_map.io_read_register_32(0x1)) & 0xFF;
        rte++;

        KLOG_INFO("IO APIC {} base {} id {} version {} RTE count {}", i, log::hex(io_map.base_address()()), id, version,
                  rte);
        if (acpi)
        {
            arch::ACPI::ioapic_source_identity identity{};
            if (id_valid && arch::ACPI::resolve_ioapic_interrupt_source(id, identity))
                KLOG_INFO("IOAPIC IOMMU source resolved id={} device_id={}", id, log::hex(identity.source.device_id));
        }
        if (!id_valid)
            KLOG_ERROR("IO APIC {} MADT ID {} is not representable by hardware", i, id);
        if (rte == 1)
        {
            KLOG_WARN("RTE count maybe invalid");
        }
        io_map.update_info(rte, id, version);
    }

    // init pit timer
    io_entry pit_entry;
    io_entry sci_entry; // System Control Interrupts
    pit_entry.dest_apic_id = static_cast<u8>(arch::APIC::local_ID());
    sci_entry.dest_apic_id = static_cast<u8>(arch::APIC::local_ID());

    if (acpi)
    {
        for (auto &override_irq : ACPI::get_override_irq_list())
        {
            KLOG_INFO("IRQ override from {} to {} bus {} flags {}", override_irq.irq_source, override_irq.gsi,
                      override_irq.bus, override_irq.flags);
            io_entry *entry = nullptr;
            if (override_irq.irq_source == 0)
            {
                // pit timer
                entry = &pit_entry;
                pit_gsi = override_irq.gsi;
            }
            else if (override_irq.irq_source == 9)
            {
                entry = &sci_entry;
                sci_gsi_number = override_irq.gsi;
            }
            if (entry)
            {
                if (override_irq.flags & 0x2)
                {
                    entry->low_level_polarity = true;
                }
                if (override_irq.flags & 0x8)
                {
                    entry->is_level_trigger_mode = true;
                }
            }
        }
    }
    else
    {
        pit_gsi = 2;
        sci_gsi_number = 9;
    }
    if (pit_gsi > 0)
    {
        if (io_irq_setup(pit_gsi, &pit_entry) == 0)
            KLOG_WARN("no interrupt vector available for PIT GSI {}", pit_gsi);
    }

    sci_interrupt_vector = io_irq_setup(sci_gsi_number, &sci_entry);
    if (sci_interrupt_vector == 0)
        KLOG_ERROR("unable to assign an interrupt vector to SCI GSI {}", sci_gsi_number);
    // The ACPI interpreter and fixed-event owner now live in platformd.  The
    // offline stage has no AcpiSciSource consumer yet, so leave the
    // level-sensitive SCI masked.  Enabling it here without a drain/ack
    // owner would create an interrupt storm and would violate the ownership
    // split; a later typed handoff must explicitly enable and rearm it.
    KLOG_INFO("ACPI SCI GSI {} remains masked until a platformd event owner is installed", sci_gsi_number);
}

u32 hpet_gsi = 2;

u8 sci_vector() { return sci_interrupt_vector; }

u32 sci_gsi() { return sci_gsi_number; }

u32 query_gsi(gsi_vector vec)
{
    switch (vec)
    {
        case gsi_vector::pit:
            return pit_gsi;
        case gsi_vector::hpet:
            return hpet_gsi;
        default:
            KLOG_PANIC("invalid gsi {}", (int)vec);
    }
    return 0;
}
bool exist(gsi_vector vec)
{
    switch (vec)
    {
        case gsi_vector::pit:
            return pit_gsi != 0;
        case gsi_vector::hpet:
            return true;
        default:
            return false;
    }
}

void set_gsi(gsi_vector vec, u32 gsi)
{
    if (vec == gsi_vector::hpet)
    {
        hpet_gsi = gsi;
    }
}

io_map_t *which(u8 index)
{
    for (auto &io_map : *io_map_vec)
    {
        if (io_map.irq_base() <= index && io_map.irq_base() + io_map.rte_count() > index)
        {
            return &io_map;
        }
    }
    return nullptr;
}

bool io_enable(u8 index)
{
    uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
    if (io_interrupt_remapping_transition)
        return false;
    auto io = which(index);
    kassert(io != nullptr, "io index not found {}", index);
    return io != nullptr && io->enable(index - io->irq_base());
}

bool io_disable(u8 index)
{
    uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
    if (io_interrupt_remapping_transition)
        return false;
    auto io = which(index);
    kassert(io != nullptr, "io index not found {}", index);
    return io != nullptr && io->disable(index - io->irq_base());
}

bool enable_interrupt_remapping_if_safe()
{
    if (arch::ACPI::interrupt_remapping_enabled())
        return true;
    if (io_map_vec == nullptr || !arch::ACPI::interrupt_remapping_ready())
        return false;

    bool routes_masked = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
        if (io_interrupt_remapping_transition)
            return false;
        io_interrupt_remapping_transition = true;
        routes_masked = true;
        for (auto &io_map : *io_map_vec)
        {
            if (!io_map.mask_configured_routes_for_remapping())
            {
                routes_masked = false;
                break;
            }
        }
    }
    const auto rollback_transition = [] {
        bool restored = true;
        for (auto &rollback_map : *io_map_vec)
            restored = rollback_map.restore_legacy_routes() && restored;
        {
            uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
            io_interrupt_remapping_transition = false;
        }
        if (!restored)
            KLOG_ERROR("IO APIC routes remain masked because interrupt-remapping rollback was incomplete");
    };
    if (!routes_masked)
    {
        rollback_transition();
        KLOG_WARN("IO APIC interrupt remapping deferred: unable to mask every configured route");
        return false;
    }

    for (auto &io_map : *io_map_vec)
    {
        if (!io_map.remap_configured_routes())
        {
            rollback_transition();
            KLOG_WARN("IO APIC interrupt remapping deferred: active source has no provider-validated route");
            return false;
        }
    }

    if (!arch::ACPI::interrupt_remapping_ready())
    {
        rollback_transition();
        KLOG_WARN("IO APIC interrupt remapping deferred: provider state became uncertain during route install");
        return false;
    }
    bool routes_ready = true;
    {
        uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
        for (auto &io_map : *io_map_vec)
        {
            if (!io_map.remapping_is_ready_to_activate())
            {
                routes_ready = false;
                break;
            }
        }
    }
    if (!routes_ready)
    {
        rollback_transition();
        KLOG_WARN("IO APIC interrupt remapping deferred: active legacy route remains");
        return false;
    }

    // Keep every IOAPIC source masked if provider activation or its completion
    // is uncertain.
    if (!arch::ACPI::enable_interrupt_remapping())
    {
        uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
        io_interrupt_remapping_transition = false;
        KLOG_ERROR("IO APIC routes remain masked after uncertain IRE enable");
        return false;
    }

    for (auto &io_map : *io_map_vec)
        io_map.restore_enabled_remapped_routes();
    {
        uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
        io_interrupt_remapping_transition = false;
    }
    KLOG_INFO("IO APIC interrupt remapping active");
    return true;
}

u8 io_irq_setup(u32 gsi, io_entry *entry)
{
    if (entry == nullptr || gsi > max_supported_gsi)
        return 0;
    if (arch::ACPI::interrupt_remapping_activation_uncertain())
        return 0;
    if (arch::ACPI::interrupt_remapping_enabled())
    {
        KLOG_ERROR("IO APIC GSI {} route creation after IRE activation is not implemented", gsi);
        return 0;
    }
    if (entry->is_remapped &&
        (!entry->is_disable || entry->delivery_mode != io_entry::mode_t::fixed || !entry->interrupt_route.valid()))
        return 0;
    uctx::RawSpinLockUninterruptibleContext guard(io_apic_access_lock);
    if (io_interrupt_remapping_transition)
        return 0;
    const auto index = static_cast<u8>(gsi);
    auto io = which(index);
    if (io == nullptr)
        return 0;
    if (!io->identity_valid())
    {
        KLOG_ERROR("IO APIC GSI {} has no verified MADT identity", gsi);
        return 0;
    }
    const auto local_index = static_cast<u8>(index - io->irq_base());
    u8 vector = io->route_vector(local_index);
    const bool allocate_vector = vector == 0;
    if (vector == 0)
        vector = irq::allocate_ioapic_vector();
    if (vector == 0)
    {
        KLOG_ERROR("no interrupt vector available for GSI {}", index);
        return 0;
    }
    const u8 configured = io->irq_setup(local_index, vector, entry);
    if (configured == 0 && allocate_vector)
    {
        (void)irq::release_ioapic_vector(vector);
    }
    return configured;
}

void io_EOI(u8 index)
{
    auto *io = io_eoi_owner[index].load(std::memory_order_acquire);
    if (io == nullptr)
        return;
    io->eoi(io_eoi_index[index].load(std::memory_order_relaxed));
}
} // namespace arch::APIC
