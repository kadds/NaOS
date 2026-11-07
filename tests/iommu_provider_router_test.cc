#include <catch2/catch_test_macros.hpp>

#include "kernel/iommu/backend.hpp"

namespace
{
struct fake_provider
{
    iommu::provider_kind kind = iommu::provider_kind::none;
    bool claims_pci = false;
    bool claims_ioapic = false;
    bool rejects_pci = false;
    bool rejects_ioapic = false;
    bool ir_ready = true;
    bool ir_enabled = false;
    bool ir_uncertain = false;
    bool fail_enable = false;
    bool fail_enable_uncertain = false;
    u32 pci_installs = 0;
    u32 ioapic_installs = 0;
    u32 retires = 0;
};

iommu::resolution_result resolve_pci(void *context, const iommu::pci_function_location &function, u64 generation,
                                     iommu::device_source_ref &source)
{
    auto &provider = *static_cast<fake_provider *>(context);
    if (provider.rejects_pci)
        return iommu::resolution_result::reject;
    if (!provider.claims_pci)
        return iommu::resolution_result::not_applicable;
    source.unit.provider = provider.kind;
    source.unit.generation = generation;
    source.unit.opaque[0] = 7;
    source.segment = function.segment;
    source.device_id = static_cast<u16>((static_cast<u16>(function.bus) << 8) |
                                        (static_cast<u16>(function.device) << 3) | function.function);
    return iommu::resolution_result::resolved;
}

iommu::resolution_result resolve_ioapic(void *context, u8 enumeration_id, u64 generation,
                                        iommu::device_source_ref &source)
{
    auto &provider = *static_cast<fake_provider *>(context);
    if (provider.rejects_ioapic)
        return iommu::resolution_result::reject;
    if (!provider.claims_ioapic)
        return iommu::resolution_result::not_applicable;
    source.unit.provider = provider.kind;
    source.unit.generation = generation;
    source.unit.opaque[0] = 11;
    source.segment = 0;
    source.device_id = static_cast<u16>(0xff00 | enumeration_id);
    return iommu::resolution_result::resolved;
}

bool install_pci(void *context, const iommu::device_source_ref &source, u32, u8 vector,
                 iommu::interrupt_route_token &token, iommu::msi_message &message, bool &outcome_uncertain)
{
    auto &provider = *static_cast<fake_provider *>(context);
    if (source.unit.provider != provider.kind)
        return false;
    provider.pci_installs++;
    token.provider = provider.kind;
    token.generation = source.unit.generation;
    token.opaque[0] = source.unit.opaque[0];
    token.opaque[1] = source.device_id;
    message = {0xfee00000, vector};
    outcome_uncertain = false;
    return true;
}

bool install_ioapic(void *context, const iommu::device_source_ref &source, u32, u8 vector, bool, bool,
                    iommu::ioapic_route_encoding &route)
{
    auto &provider = *static_cast<fake_provider *>(context);
    if (source.unit.provider != provider.kind)
        return false;
    provider.ioapic_installs++;
    route.token.provider = provider.kind;
    route.token.generation = source.unit.generation;
    route.token.opaque[0] = source.unit.opaque[0];
    route.token.opaque[1] = source.device_id;
    route.redirection_entry = vector | (1ull << 16);
    return true;
}

bool route_matches_pci(void *context, const iommu::interrupt_route_token &token, const iommu::device_source_ref &source)
{
    const auto &provider = *static_cast<fake_provider *>(context);
    return token.provider == provider.kind && token.generation == source.unit.generation &&
           token.opaque[0] == source.unit.opaque[0] && token.opaque[1] == source.device_id;
}

bool retire_route(void *context, const iommu::interrupt_route_token &token)
{
    auto &provider = *static_cast<fake_provider *>(context);
    if (token.provider != provider.kind)
        return false;
    provider.retires++;
    return true;
}

bool interrupt_remapping_ready(void *context) { return static_cast<fake_provider *>(context)->ir_ready; }

bool enable_interrupt_remapping(void *context, bool &outcome_uncertain)
{
    auto &provider = *static_cast<fake_provider *>(context);
    outcome_uncertain = provider.fail_enable_uncertain;
    if (provider.fail_enable)
        return false;
    provider.ir_enabled = true;
    return true;
}

bool interrupt_remapping_enabled(void *context) { return static_cast<fake_provider *>(context)->ir_enabled; }
bool interrupt_remapping_uncertain(void *context) { return static_cast<fake_provider *>(context)->ir_uncertain; }

iommu::dma_domain_create_result create_dma_domain(void *, const iommu::device_source_ref &,
                                                  const pci::function_identity &, process_id,
                                                  handle_t<pci::dma_domain> &, handle_t<pci::dma_fault_source> &)
{
    return {NA_STATUS_NOT_SUPPORTED, false};
}

iommu::provider_ops make_ops(fake_provider &provider)
{
    return {provider.kind,
            &provider,
            resolve_pci,
            resolve_ioapic,
            install_pci,
            install_ioapic,
            route_matches_pci,
            retire_route,
            interrupt_remapping_ready,
            enable_interrupt_remapping,
            interrupt_remapping_enabled,
            interrupt_remapping_uncertain,
            create_dma_domain};
}

iommu::provider_ops make_requester_scoped_ops(fake_provider &provider)
{
    return {provider.kind,     &provider,
            resolve_pci,       resolve_ioapic,
            install_pci,       install_ioapic,
            route_matches_pci, retire_route,
            nullptr,           nullptr,
            nullptr,           interrupt_remapping_uncertain,
            create_dma_domain, iommu::interrupt_activation_scope::per_requester};
}
} // namespace

TEST_CASE("provider router dispatches source-bound routes and retirement by provider token", "[iommu][interrupt]")
{
    fake_provider intel{iommu::provider_kind::intel_vtd};
    intel.claims_pci = true;
    intel.claims_ioapic = true;
    const auto intel_ops = make_ops(intel);
    iommu::provider_router router;
    REQUIRE(router.register_provider(intel_ops));

    const iommu::pci_function_location function{0, 3, 0, 0};
    iommu::interrupt_route_token token{};
    iommu::msi_message message{};
    bool outcome_uncertain = false;
    REQUIRE(router.install_pci_msi(function, 19, 0x2a, 0x45, token, message, outcome_uncertain));
    CHECK_FALSE(outcome_uncertain);
    CHECK(token.provider == iommu::provider_kind::intel_vtd);
    CHECK(token.generation == 19);
    CHECK(message.data == 0x45);
    CHECK(intel.pci_installs == 1);

    iommu::device_source_ref source{};
    REQUIRE(router.resolve_pci(function, 19, source));
    CHECK(router.route_matches_pci(token, source));
    CHECK_FALSE(router.route_matches_pci(token, iommu::device_source_ref{}));

    iommu::ioapic_route_encoding io_route{};
    REQUIRE(router.install_ioapic(2, 19, 0x2a, 0x46, true, true, io_route));
    CHECK(io_route.token.provider == iommu::provider_kind::intel_vtd);
    CHECK(io_route.redirection_entry == (0x46 | (1ull << 16)));
    REQUIRE(router.retire(token));
    REQUIRE(router.retire(io_route.token));
    CHECK(intel.retires == 2);
}

TEST_CASE("provider router fails closed on overlapping or rejected firmware scopes", "[iommu][interrupt]")
{
    fake_provider intel{iommu::provider_kind::intel_vtd};
    fake_provider amd{iommu::provider_kind::amd_vi};
    intel.claims_pci = true;
    amd.claims_pci = true;
    const auto intel_ops = make_ops(intel);
    const auto amd_ops = make_ops(amd);
    iommu::provider_router ambiguous_router;
    REQUIRE(ambiguous_router.register_provider(intel_ops));
    REQUIRE(ambiguous_router.register_provider(amd_ops));

    iommu::device_source_ref source{};
    CHECK_FALSE(ambiguous_router.resolve_pci({0, 3, 0, 0}, 23, source));
    CHECK(intel.pci_installs == 0);
    CHECK(amd.pci_installs == 0);

    amd.claims_pci = false;
    intel.rejects_pci = true;
    iommu::provider_router rejected_router;
    REQUIRE(rejected_router.register_provider(intel_ops));
    REQUIRE(rejected_router.register_provider(amd_ops));
    CHECK_FALSE(rejected_router.resolve_pci({0, 3, 0, 0}, 23, source));
}

TEST_CASE("provider router enables every registered provider or records uncertain activation", "[iommu][interrupt]")
{
    fake_provider intel{iommu::provider_kind::intel_vtd};
    fake_provider amd{iommu::provider_kind::amd_vi};
    intel.ir_ready = true;
    amd.ir_ready = false;
    const auto intel_ops = make_ops(intel);
    const auto amd_ops = make_ops(amd);
    iommu::provider_router router;
    REQUIRE(router.register_provider(intel_ops));
    REQUIRE(router.register_provider(amd_ops));

    CHECK_FALSE(router.interrupt_remapping_ready());
    CHECK_FALSE(router.enable_interrupt_remapping());
    CHECK_FALSE(intel.ir_enabled);

    amd.ir_ready = true;
    amd.fail_enable = true;
    amd.fail_enable_uncertain = true;
    CHECK(router.interrupt_remapping_ready());
    CHECK_FALSE(router.enable_interrupt_remapping());
    CHECK(router.interrupt_remapping_activation_uncertain());
    CHECK_FALSE(router.interrupt_remapping_enabled());
}

TEST_CASE("provider router activates requester-scoped IOAPIC providers without global IRE hooks",
          "[iommu][interrupt][ioapic]")
{
    fake_provider amd{iommu::provider_kind::amd_vi};
    amd.claims_ioapic = true;
    const auto amd_ops = make_requester_scoped_ops(amd);
    iommu::provider_router router;
    REQUIRE(router.register_provider(amd_ops));

    iommu::ioapic_route_encoding route{};
    REQUIRE(router.install_ioapic(7, 31, 0x2a, 0x45, true, true, route));
    CHECK(route.token.provider == iommu::provider_kind::amd_vi);
    CHECK(amd.ioapic_installs == 1);
    CHECK(router.interrupt_remapping_ready());
    CHECK_FALSE(router.interrupt_remapping_enabled());

    REQUIRE(router.enable_interrupt_remapping());
    CHECK(router.interrupt_remapping_enabled());
    CHECK_FALSE(amd.ir_enabled); // The AMD provider has no unit-global IRE operation.
    CHECK_FALSE(amd.ir_uncertain);
}

TEST_CASE("provider router withholds requester-scoped activation when provider state is uncertain",
          "[iommu][interrupt][ioapic][failure]")
{
    fake_provider amd{iommu::provider_kind::amd_vi};
    amd.claims_ioapic = true;
    amd.ir_uncertain = true;
    const auto amd_ops = make_requester_scoped_ops(amd);
    iommu::provider_router router;
    REQUIRE(router.register_provider(amd_ops));

    CHECK_FALSE(router.interrupt_remapping_ready());
    CHECK_FALSE(router.enable_interrupt_remapping());
    CHECK(router.interrupt_remapping_activation_uncertain());
    CHECK_FALSE(router.interrupt_remapping_enabled());
}
