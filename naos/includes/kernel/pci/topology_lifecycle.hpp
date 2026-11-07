#pragma once

#include "kernel/types.hpp"
#include <atomic>

namespace pci
{

struct platform_generation_lineage
{
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 aml_namespace_generation = 0;
    u64 platform_topology_generation = 0;
    u64 pci_fabric_generation = 0;
};

/// Kernel authority for the one immutable PCI topology incarnation available
/// during a boot. SCI-consumer failure revokes it permanently; a new boot is
/// required before another generation can be accepted.
class topology_lifecycle
{
  public:
    bool initialize(u64 boot_epoch, u64 firmware_generation, u64 pci_fabric_generation) noexcept
    {
        if (boot_epoch == 0 || firmware_generation == 0 || pci_fabric_generation == 0)
            return false;

        u8 expected = 0;
        if (!initialization_state_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                                           std::memory_order_acquire))
            return false;

        boot_epoch_ = boot_epoch;
        firmware_generation_ = firmware_generation;
        pci_fabric_generation_ = pci_fabric_generation;
        initialization_state_.store(2, std::memory_order_release);
        return true;
    }

    bool bind_generation(u64 boot_epoch, u64 firmware_generation, u64 aml_namespace_generation,
                         u64 pci_fabric_generation, u64 platform_topology_generation) noexcept
    {
        if (!matches_firmware(boot_epoch, firmware_generation, pci_fabric_generation) ||
            !matches_platform_generation(boot_epoch, firmware_generation, aml_namespace_generation,
                                         platform_topology_generation) ||
            invalidated_.load(std::memory_order_acquire))
            return false;

        u64 expected = 0;
        (void)platform_topology_generation_.compare_exchange_strong(
            expected, platform_topology_generation, std::memory_order_acq_rel, std::memory_order_acquire);
        return !invalidated_.load(std::memory_order_acquire) &&
               platform_topology_generation_.load(std::memory_order_acquire) == platform_topology_generation;
    }

    bool is_current(u64 boot_epoch, u64 firmware_generation, u64 aml_namespace_generation, u64 pci_fabric_generation,
                    u64 platform_topology_generation) const noexcept
    {
        return matches_firmware(boot_epoch, firmware_generation, pci_fabric_generation) &&
               matches_platform_generation(boot_epoch, firmware_generation, aml_namespace_generation,
                                           platform_topology_generation) &&
               !invalidated_.load(std::memory_order_acquire);
    }

    bool bind_platform_generation(u64 boot_epoch, u64 firmware_generation, u64 aml_namespace_generation,
                                  u64 platform_topology_generation) noexcept
    {
        if (!matches_firmware(boot_epoch, firmware_generation, pci_fabric_generation_) ||
            aml_namespace_generation == 0 || platform_topology_generation == 0 ||
            invalidated_.load(std::memory_order_acquire))
            return false;

        u8 expected = 0;
        if (platform_generation_state_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
        {
            aml_namespace_generation_.store(aml_namespace_generation, std::memory_order_relaxed);
            platform_topology_generation_.store(platform_topology_generation, std::memory_order_relaxed);
            platform_generation_state_.store(2, std::memory_order_release);
            return true;
        }

        if (platform_generation_state_.load(std::memory_order_acquire) != 2)
            return false;
        return matches_platform_generation(boot_epoch, firmware_generation, aml_namespace_generation,
                                           platform_topology_generation) &&
               !invalidated_.load(std::memory_order_acquire);
    }

    bool get_platform_generation_lineage(platform_generation_lineage &lineage) const noexcept
    {
        if (!is_initialized() || platform_generation_state_.load(std::memory_order_acquire) != 2)
            return false;
        lineage = {boot_epoch_, firmware_generation_, aml_namespace_generation_.load(std::memory_order_relaxed),
                   platform_topology_generation_.load(std::memory_order_relaxed), pci_fabric_generation_};
        return lineage.boot_epoch != 0 && lineage.firmware_generation != 0 && lineage.aml_namespace_generation != 0 &&
               lineage.platform_topology_generation != 0 && lineage.pci_fabric_generation != 0;
    }

    bool is_platform_generation_current(const platform_generation_lineage &lineage) const noexcept
    {
        return matches_platform_generation(lineage.boot_epoch, lineage.firmware_generation,
                                           lineage.aml_namespace_generation, lineage.platform_topology_generation) &&
               lineage.pci_fabric_generation == pci_fabric_generation_ && !invalidated_.load(std::memory_order_acquire);
    }

    bool invalidate(u64 boot_epoch, u64 firmware_generation) noexcept
    {
        if (initialization_state_.load(std::memory_order_acquire) != 2 || boot_epoch != boot_epoch_ ||
            firmware_generation != firmware_generation_)
            return false;
        bool expected = false;
        return invalidated_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
    }

    bool is_initialized() const noexcept { return initialization_state_.load(std::memory_order_acquire) == 2; }
    bool is_invalidated() const noexcept { return invalidated_.load(std::memory_order_acquire); }

  private:
    bool matches_platform_generation(u64 boot_epoch, u64 firmware_generation, u64 aml_namespace_generation,
                                     u64 platform_topology_generation) const noexcept
    {
        return initialization_state_.load(std::memory_order_acquire) == 2 && boot_epoch == boot_epoch_ &&
               firmware_generation == firmware_generation_ && aml_namespace_generation != 0 &&
               platform_topology_generation != 0 && platform_generation_state_.load(std::memory_order_acquire) == 2 &&
               aml_namespace_generation_.load(std::memory_order_relaxed) == aml_namespace_generation &&
               platform_topology_generation_.load(std::memory_order_relaxed) == platform_topology_generation;
    }

    bool matches_firmware(u64 boot_epoch, u64 firmware_generation, u64 pci_fabric_generation) const noexcept
    {
        return initialization_state_.load(std::memory_order_acquire) == 2 && boot_epoch == boot_epoch_ &&
               firmware_generation == firmware_generation_ && pci_fabric_generation == pci_fabric_generation_;
    }

    u64 boot_epoch_ = 0;
    u64 firmware_generation_ = 0;
    u64 pci_fabric_generation_ = 0;
    std::atomic<u64> aml_namespace_generation_{0};
    std::atomic<u64> platform_topology_generation_{0};
    std::atomic<u8> platform_generation_state_{0}; // 0=unbound, 1=writing, 2=immutable
    std::atomic<u8> initialization_state_{0};      // 0=uninitialized, 1=initializing, 2=ready
    std::atomic<bool> invalidated_{false};
};

/// The PCI topology incarnation shared by discovery capabilities and platform
/// lifecycle owners. The instance is defined by the PCI subsystem.
topology_lifecycle &topology_lifetime() noexcept;

} // namespace pci
