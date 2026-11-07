#pragma once

#include "kernel/mm/data_plane_dma.hpp"
#include "kernel/types.hpp"
#include "naos/abi.h"

template <typename T> class handle_t;
namespace pci
{
class dma_domain;
class dma_fault_source;
struct function_identity;
} // namespace pci

namespace iommu
{
/// Hardware providers are selected from validated firmware inventories. The
/// tag is carried only by kernel-internal route tokens and never grants
/// authority by itself.
enum class provider_kind : u8
{
    none,
    intel_vtd,
    amd_vi,
};

/// The PCI MSI payload is produced by the selected IOMMU provider. Generic
/// PCI code may store and verify it, but must not interpret provider bits.
struct msi_message
{
    u64 address = 0;
    u32 data = 0;
};

/// Provider-owned route identity. `opaque` words are meaningful only to the
/// backend named by `provider`; common IRQ code only copies the token back.
struct interrupt_route_token
{
    provider_kind provider = provider_kind::none;
    u64 generation = 0;
    u64 opaque[2]{};

    constexpr bool valid() const noexcept
    {
        return generation != 0 && (provider == provider_kind::intel_vtd || provider == provider_kind::amd_vi);
    }
};

/// Fully encoded IOAPIC redirection entry for a provider-managed route.
/// Standard mask/EOI handling remains in the IOAPIC driver.
struct ioapic_route_encoding
{
    interrupt_route_token token{};
    u64 redirection_entry = 0;
};

struct pci_function_location
{
    u16 segment = 0;
    u8 bus = 0;
    u8 device = 0;
    u8 function = 0;
};

struct unit_ref
{
    provider_kind provider = provider_kind::none;
    u64 generation = 0;
    u64 opaque[2]{};

    constexpr bool valid() const noexcept
    {
        return generation != 0 && (provider == provider_kind::intel_vtd || provider == provider_kind::amd_vi);
    }
};

struct device_source_ref
{
    unit_ref unit{};
    u16 segment = 0;
    u16 device_id = 0;

    constexpr bool valid() const noexcept { return unit.valid(); }
};

using pci_config_read32_callback = bool (*)(void *context, u16 segment, u8 bus, u8 device, u8 function, u16 offset,
                                            u32 &value);

struct pci_requester_platform_ops
{
    bool (*bus_master_enabled)(void *context, u16 segment, u16 requester_id, bool &enabled) = nullptr;
    bool (*requester_present)(void *context, u16 segment, u16 requester_id, bool &present) = nullptr;
    pci_config_read32_callback read_config32 = nullptr;
};

struct dma_domain_create_result
{
    na_status_t status = NA_STATUS_NOT_SUPPORTED;
    bool outcome_uncertain = false;
};

enum class fault_delivery_mode : u8
{
    none,
    interrupt,
    polling,
};

struct provider_initialization_report
{
    na_status_t status = NA_STATUS_NOT_SUPPORTED;
    bool state_uncertain = false;
    fault_delivery_mode fault_delivery = fault_delivery_mode::none;
    u32 selected_units = 0;
    u32 active_units = 0;
    u32 invalidation_ready_units = 0;
    u32 interrupt_remapping_units = 0;
};

struct provider_bootstrap_report
{
    provider_initialization_report providers[3]{};
};

using pci_bridge_info_reader = bool (*)(void *context, u16 segment, u8 bus, u8 device, u8 function, u8 &header_type,
                                        u8 &primary_bus, u8 &secondary_bus, u8 &subordinate_bus);
using acquire_register_aperture = bool (*)(void *context, u64 physical_base, u64 length, u64 generation, u64 &lease_id,
                                           u64 &kernel_virtual_base);
using release_register_aperture = bool (*)(void *context, u64 physical_base, u64 length, u64 generation, u64 lease_id,
                                           u64 kernel_virtual_base);
using physical_range_validator = bool (*)(void *context, u64 physical_base, u64 length);

/// Opaque vendor domain construction and page lifecycle. ACPI owns requester
/// authorization and the public PciDmaDomain capability; adapters own the
/// vendor state behind `backend_state` and these callbacks. If creation fails
/// after hardware state may be visible, the adapter returns that retained
/// state and sets `outcome_uncertain`; the caller must not attempt teardown.
struct provider_dma_domain_adapter_ops
{
    na_status_t (*create)(const device_source_ref &source, const pci::function_identity &identity, process_id owner,
                          handle_t<pci::dma_fault_source> &fault_source, void *&backend_state, u8 &address_width,
                          u32 &unit_index, bool &outcome_uncertain) = nullptr;
    na_status_t (*map_page)(void *backend_state, u64 iova, u64 physical_address,
                            naos::data_plane::dma_direction direction, bool &outcome_uncertain) = nullptr;
    na_status_t (*unmap_page)(void *backend_state, u64 iova, bool &outcome_uncertain) = nullptr;
    na_status_t (*destroy)(void *backend_state, bool &outcome_uncertain) = nullptr;
    bool (*unit_state_uncertain)(u32 unit_index) = nullptr;
    bool (*ready_for_bus_master)() = nullptr;
};

enum class resolution_result : u8
{
    not_applicable,
    resolved,
    reject,
};

using pci_source_resolver = resolution_result (*)(void *context, const pci_function_location &function, u64 generation,
                                                  device_source_ref &source);
using ioapic_source_resolver = resolution_result (*)(void *context, u8 enumeration_id, u64 generation,
                                                     device_source_ref &source);
using ioapic_match_counter = u32 (*)(void *context, u8 enumeration_id);
using dma_domain_creator = dma_domain_create_result (*)(void *context, const device_source_ref &source,
                                                        const pci::function_identity &identity, process_id owner,
                                                        handle_t<pci::dma_domain> &domain,
                                                        handle_t<pci::dma_fault_source> &fault_source);

struct provider_bootstrap_config
{
    void *firmware_inventory = nullptr;
    bool pci_bus_mastering_quiesced = false;
    const u8 *ioapic_enumeration_ids = nullptr;
    u32 ioapic_source_count = 0;
    pci_requester_platform_ops pci_requesters{};
    pci_bridge_info_reader read_bridge_info = nullptr;
    ioapic_match_counter count_madt_ioapic_matches = nullptr;
    void *platform_context = nullptr;
    dma_domain_creator create_dma_domain = nullptr;
    acquire_register_aperture acquire_registers = nullptr;
    release_register_aperture release_registers = nullptr;
    void *register_aperture_context = nullptr;
    physical_range_validator validate_device_range = nullptr;
    physical_range_validator validate_register_range = nullptr;
    void *range_validation_context = nullptr;
};

enum class interrupt_activation_scope : u8
{
    unit_global,
    per_requester,
};

inline constexpr u32 max_ioapic_sources = 64;

/// A provider resolves firmware identity before performing any hardware
/// mutation. `reject` means its inventory claims the device but cannot prove a
/// unique source; the router must fail closed without trying another provider.
struct provider_ops
{
    provider_kind kind = provider_kind::none;
    void *context = nullptr;
    pci_source_resolver resolve_pci = nullptr;
    ioapic_source_resolver resolve_ioapic = nullptr;
    bool (*install_pci_msi)(void *context, const device_source_ref &source, u32 destination_apic_id, u8 vector,
                            interrupt_route_token &token, msi_message &message, bool &outcome_uncertain) = nullptr;
    bool (*install_ioapic)(void *context, const device_source_ref &source, u32 destination_apic_id, u8 vector,
                           bool low_polarity, bool level_triggered, ioapic_route_encoding &route) = nullptr;
    bool (*route_matches_pci)(void *context, const interrupt_route_token &token,
                              const device_source_ref &source) = nullptr;
    bool (*retire_route)(void *context, const interrupt_route_token &token) = nullptr;
    bool (*interrupt_remapping_ready)(void *context) = nullptr;
    bool (*enable_interrupt_remapping)(void *context, bool &outcome_uncertain) = nullptr;
    bool (*interrupt_remapping_enabled)(void *context) = nullptr;
    bool (*interrupt_remapping_activation_uncertain)(void *context) = nullptr;
    dma_domain_creator create_dma_domain = nullptr;
    interrupt_activation_scope activation_scope = interrupt_activation_scope::unit_global;
};

/// Fixed-capacity, boot-initialized router for provider-owned DMA and interrupt
/// operations. A provider may expose DMA before it implements interrupt
/// routing. Unit-global activation and requester-scoped activation are kept
/// distinct so an AMD DTE route does not masquerade as a global IRE switch.
class provider_router
{
  public:
    static constexpr u32 max_providers = 2;

    bool register_provider(const provider_ops &provider)
    {
        if ((provider.kind != provider_kind::intel_vtd && provider.kind != provider_kind::amd_vi) ||
            provider.resolve_pci == nullptr || provider.create_dma_domain == nullptr)
            return false;
        const bool has_interrupt_operations =
            provider.resolve_ioapic != nullptr || provider.install_pci_msi != nullptr ||
            provider.install_ioapic != nullptr || provider.route_matches_pci != nullptr ||
            provider.retire_route != nullptr || provider.interrupt_remapping_ready != nullptr ||
            provider.enable_interrupt_remapping != nullptr || provider.interrupt_remapping_enabled != nullptr ||
            provider.interrupt_remapping_activation_uncertain != nullptr;
        if (has_interrupt_operations && !has_complete_interrupt_operations(provider))
            return false;
        for (u32 index = 0; index < count_; ++index)
        {
            if (providers_[index].kind == provider.kind)
                return false;
        }
        if (count_ == max_providers)
            return false;
        if (provider.activation_scope == interrupt_activation_scope::unit_global && provider.install_pci_msi != nullptr)
            unit_global_interrupt_provider_count_++;
        providers_[count_++] = provider;
        return true;
    }

    bool resolve_pci(const pci_function_location &function, u64 generation, device_source_ref &source) const
    {
        source = {};
        return resolve(
                   [&function, generation](const provider_ops &provider, device_source_ref &candidate) {
                       return provider.resolve_pci(provider.context, function, generation, candidate);
                   },
                   source) &&
               source.unit.generation == generation && source.segment == function.segment;
    }

    bool resolve_ioapic(u8 enumeration_id, u64 generation, device_source_ref &source) const
    {
        source = {};
        return resolve(
                   [enumeration_id, generation](const provider_ops &provider, device_source_ref &candidate) {
                       if (provider.resolve_ioapic == nullptr)
                           return resolution_result::not_applicable;
                       return provider.resolve_ioapic(provider.context, enumeration_id, generation, candidate);
                   },
                   source) &&
               source.unit.generation == generation;
    }

    bool install_pci_msi(const pci_function_location &function, u64 generation, u32 destination_apic_id, u8 vector,
                         interrupt_route_token &token, msi_message &message, bool &outcome_uncertain) const
    {
        token = {};
        message = {};
        outcome_uncertain = false;
        device_source_ref source{};
        if (!resolve_pci(function, generation, source))
            return false;
        const auto *provider = find(source.unit.provider);
        if (provider == nullptr || provider->install_pci_msi == nullptr ||
            !provider->install_pci_msi(provider->context, source, destination_apic_id, vector, token, message,
                                       outcome_uncertain))
            return false;
        if (!token.valid() || token.provider != source.unit.provider || token.generation != generation ||
            message.data > 0xffff)
        {
            token = {};
            message = {};
            outcome_uncertain = true;
            return false;
        }
        return true;
    }

    bool install_ioapic(u8 enumeration_id, u64 generation, u32 destination_apic_id, u8 vector, bool low_polarity,
                        bool level_triggered, ioapic_route_encoding &route) const
    {
        route = {};
        device_source_ref source{};
        if (!resolve_ioapic(enumeration_id, generation, source))
            return false;
        const auto *provider = find(source.unit.provider);
        if (provider == nullptr || provider->install_ioapic == nullptr ||
            !provider->install_ioapic(provider->context, source, destination_apic_id, vector, low_polarity,
                                      level_triggered, route))
            return false;
        if (!route.token.valid() || route.token.provider != source.unit.provider ||
            route.token.generation != generation)
        {
            route = {};
            return false;
        }
        return true;
    }

    bool retire(const interrupt_route_token &token) const
    {
        if (!token.valid())
            return false;
        const auto *provider = find(token.provider);
        return provider != nullptr && provider->retire_route != nullptr &&
               provider->retire_route(provider->context, token);
    }

    dma_domain_create_result create_dma_domain(const pci_function_location &function, u64 generation,
                                               const pci::function_identity &identity, process_id owner,
                                               handle_t<pci::dma_domain> &domain,
                                               handle_t<pci::dma_fault_source> &fault_source) const
    {
        device_source_ref source{};
        if (!resolve_pci(function, generation, source))
            return {NA_STATUS_NOT_SUPPORTED, false};
        const auto *provider = find(source.unit.provider);
        return provider == nullptr
                   ? dma_domain_create_result{NA_STATUS_NOT_SUPPORTED, false}
                   : provider->create_dma_domain(provider->context, source, identity, owner, domain, fault_source);
    }

    bool route_matches_pci(const interrupt_route_token &token, const device_source_ref &source) const
    {
        if (!token.valid() || !source.valid() || token.provider != source.unit.provider ||
            token.generation != source.unit.generation)
            return false;
        const auto *provider = find(token.provider);
        return provider != nullptr && provider->route_matches_pci != nullptr &&
               provider->route_matches_pci(provider->context, token, source);
    }

    bool interrupt_remapping_ready() const
    {
        const bool has_requester_scoped_ioapic_provider = requester_scoped_ioapic_provider_present();
        if ((unit_global_interrupt_provider_count_ == 0 && !has_requester_scoped_ioapic_provider) ||
            interrupt_remapping_activation_uncertain())
            return false;
        for (u32 index = 0; index < count_; ++index)
        {
            if (!uses_unit_global_activation(providers_[index]))
                continue;
            if (!providers_[index].interrupt_remapping_ready(providers_[index].context))
                return false;
        }
        return true;
    }

    bool enable_interrupt_remapping()
    {
        if (!interrupt_remapping_ready())
            return false;
        u32 enabled_count = 0;
        for (u32 index = 0; index < count_; ++index)
        {
            if (!uses_unit_global_activation(providers_[index]))
                continue;
            bool outcome_uncertain = false;
            if (!providers_[index].enable_interrupt_remapping(providers_[index].context, outcome_uncertain))
            {
                activation_uncertain_ = outcome_uncertain || enabled_count != 0;
                return false;
            }
            enabled_count++;
        }
        if (enabled_count != unit_global_interrupt_provider_count_)
            return false;
        requester_scoped_ioapic_active_ = requester_scoped_ioapic_provider_present();
        activation_uncertain_ = false;
        return true;
    }

    bool interrupt_remapping_enabled() const
    {
        if (interrupt_remapping_activation_uncertain())
            return false;
        bool global_providers_enabled = unit_global_interrupt_provider_count_ != 0;
        for (u32 index = 0; index < count_; ++index)
        {
            if (!uses_unit_global_activation(providers_[index]))
                continue;
            if (!providers_[index].interrupt_remapping_enabled(providers_[index].context))
                global_providers_enabled = false;
        }
        if (requester_scoped_ioapic_provider_present())
            return requester_scoped_ioapic_active_ &&
                   (unit_global_interrupt_provider_count_ == 0 || global_providers_enabled);
        return global_providers_enabled;
    }

    bool interrupt_remapping_activation_uncertain() const
    {
        if (activation_uncertain_)
            return true;
        for (u32 index = 0; index < count_; ++index)
        {
            if (providers_[index].interrupt_remapping_activation_uncertain == nullptr)
                continue;
            if (providers_[index].interrupt_remapping_activation_uncertain(providers_[index].context))
                return true;
        }
        return false;
    }

  private:
    bool requester_scoped_ioapic_provider_present() const
    {
        for (u32 index = 0; index < count_; ++index)
        {
            const auto &provider = providers_[index];
            if (provider.activation_scope == interrupt_activation_scope::per_requester &&
                provider.resolve_ioapic != nullptr && provider.install_ioapic != nullptr)
                return true;
        }
        return false;
    }

    static bool uses_unit_global_activation(const provider_ops &provider)
    {
        return provider.activation_scope == interrupt_activation_scope::unit_global &&
               provider.install_pci_msi != nullptr;
    }

    static bool has_complete_interrupt_operations(const provider_ops &provider)
    {
        const bool has_ioapic_operations = provider.resolve_ioapic != nullptr || provider.install_ioapic != nullptr;
        const bool complete_pci_operations = provider.install_pci_msi != nullptr &&
                                             provider.route_matches_pci != nullptr && provider.retire_route != nullptr;
        const bool complete_ioapic_operations =
            !has_ioapic_operations || (provider.resolve_ioapic != nullptr && provider.install_ioapic != nullptr);
        const bool has_activation_control = provider.interrupt_remapping_ready != nullptr ||
                                            provider.enable_interrupt_remapping != nullptr ||
                                            provider.interrupt_remapping_enabled != nullptr;
        if (!complete_pci_operations || !complete_ioapic_operations)
            return false;
        if (provider.activation_scope == interrupt_activation_scope::per_requester)
            return !has_activation_control &&
                   (!has_ioapic_operations || provider.interrupt_remapping_activation_uncertain != nullptr);
        return provider.interrupt_remapping_ready != nullptr && provider.enable_interrupt_remapping != nullptr &&
               provider.interrupt_remapping_enabled != nullptr &&
               provider.interrupt_remapping_activation_uncertain != nullptr;
    }

    template <typename Resolver> bool resolve(Resolver resolver, device_source_ref &source) const
    {
        const provider_ops *matched_provider = nullptr;
        device_source_ref matched_source{};
        for (u32 index = 0; index < count_; ++index)
        {
            device_source_ref candidate{};
            const auto &provider = providers_[index];
            const auto result = resolver(provider, candidate);
            if (result == resolution_result::not_applicable)
                continue;
            if (result != resolution_result::resolved || !candidate.valid() || candidate.unit.provider != provider.kind)
                return false;
            if (matched_provider != nullptr)
                return false;
            matched_provider = &provider;
            matched_source = candidate;
        }
        if (matched_provider == nullptr)
            return false;
        source = matched_source;
        return true;
    }

    const provider_ops *find(provider_kind kind) const
    {
        for (u32 index = 0; index < count_; ++index)
        {
            if (providers_[index].kind == kind)
                return &providers_[index];
        }
        return nullptr;
    }

    provider_ops providers_[max_providers]{};
    u32 count_ = 0;
    u32 unit_global_interrupt_provider_count_ = 0;
    bool activation_uncertain_ = false;
    bool requester_scoped_ioapic_active_ = false;
};

/// Kernel-wide, boot-scoped provider registry. ACPI discovers/registers the
/// validated backends; PCI and IRQ consumers use the neutral routing surface.
provider_router &kernel_provider_router() noexcept;
bool register_provider_dma_domain_adapter(provider_kind provider, const provider_dma_domain_adapter_ops &ops);
na_status_t create_provider_dma_domain_backend(provider_kind provider, const device_source_ref &source,
                                               const pci::function_identity &identity, process_id owner,
                                               handle_t<pci::dma_fault_source> &fault_source, void *&backend_state,
                                               u8 &address_width, u32 &unit_index, bool &outcome_uncertain);
na_status_t map_provider_dma_domain_page(provider_kind provider, void *backend_state, u64 iova, u64 physical_address,
                                         naos::data_plane::dma_direction direction, bool &outcome_uncertain);
na_status_t unmap_provider_dma_domain_page(provider_kind provider, void *backend_state, u64 iova,
                                           bool &outcome_uncertain);
na_status_t destroy_provider_dma_domain_backend(provider_kind provider, void *backend_state, bool &outcome_uncertain);
bool provider_unit_state_uncertain(provider_kind provider, u32 unit_index);
bool provider_dma_domain_ready_for_bus_master(provider_kind provider);
provider_initialization_report initialize_provider_backend(provider_kind provider,
                                                           const provider_bootstrap_config &configuration);
provider_bootstrap_report initialize_firmware_provider_backends(const provider_bootstrap_config &configuration);
} // namespace iommu
