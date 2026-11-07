#pragma once

#include "kernel/pci/dma_authority.hpp"
#include "kernel/pci/firmware.hpp"
#include "kernel/types.hpp"

namespace pci
{
class dma_domain;

/// A non-forgeable authorization minted only by a live kernel DMA domain.
/// The command helper still rechecks policy immediately before touching ECAM.
class bus_master_enable_authority
{
  public:
    bus_master_enable_authority() = delete;

  private:
    bus_master_enable_authority(u32 segment, u8 bus, u8 device, u8 function, dma_authority::bus_master_state state)
        : segment_(segment)
        , bus_(bus)
        , device_(device)
        , function_(function)
        , state_(state)
    {
    }

    friend class dma_domain;

  public:
    u32 segment() const { return segment_; }
    u8 bus() const { return bus_; }
    u8 device() const { return device_; }
    u8 function() const { return function_; }
    const dma_authority::bus_master_state &state() const { return state_; }

  private:
    u32 segment_;
    u8 bus_;
    u8 device_;
    u8 function_;
    dma_authority::bus_master_state state_;
};
} // namespace pci

namespace pci::dma_quiesce
{
enum class error : u8
{
    none,
    invalid_region,
    mapping_unavailable,
    invalid_operations,
    config_read_failed,
    command_read_failed,
    command_write_failed,
    bus_master_still_enabled,
    bus_master_still_disabled,
    invalid_device_id,
    domain_required,
    domain_not_ready,
    no_iommu_policy,
    domain_state_uncertain,
};

struct result
{
    u32 functions_seen = 0;
    u32 bus_masters_disabled = 0;
};

/// Narrow kernel-internal PCI config operations used only to disable DMA.
/// This is not a config-space or MMIO capability exposed to userland.
struct config_ops
{
    bool (*read_identity)(void *context, u8 bus, u8 device, u8 function, u16 &vendor, u8 &header_type);
    bool (*read_command)(void *context, u8 bus, u8 device, u8 function, u16 &command);
    bool (*write_command)(void *context, u8 bus, u8 device, u8 function, u16 command);
    void *context;
    u32 segment = 0;
};

/// Clear PCI Command.BusMasterEnable on every present function in one MCFG
/// allocation. Translation must not be enabled unless the full scan succeeds.
error quiesce_region(const ecam_region &region, const config_ops &operations, result &scan_result) noexcept;

/// Disable PCI Command.BusMasterEnable without any additional authority. This
/// overload deliberately rejects enable requests.
error set_bus_master(const config_ops &operations, u8 bus, u8 device, u8 function, bool enabled) noexcept;
/// Enable BME only with an authority minted by pci::dma_domain. This API
/// binds the authority to one BDF and applies the fail-closed domain policy.
error set_bus_master(const config_ops &operations, const pci::bus_master_enable_authority &authority) noexcept;
} // namespace pci::dma_quiesce
