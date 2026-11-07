#include <catch2/catch_test_macros.hpp>

#include "kernel/pci/osc_authority.hpp"

namespace
{
constexpr u32 supported_os_feature = 1u << 3;
// Positive-path fixture for the pure validator; production enables no control bits.
constexpr u32 synthetic_control_feature = 1u << 4;

pci::root_feature_scope make_scope() { return {0x1122, 7, 3, 5, 9, 11, 13, 17, 32, 63}; }

pci::root_feature_evidence make_evidence(const pci::root_feature_scope &scope)
{
    pci::root_feature_evidence result{};
    static_cast<pci::root_feature_scope &>(result) = scope;
    result.requested_support = supported_os_feature;
    result.requested_control = synthetic_control_feature;
    result.query_support = supported_os_feature;
    result.query_granted_control = synthetic_control_feature;
    result.commit_support = supported_os_feature;
    result.granted_control = synthetic_control_feature;
    return result;
}

pci::root_feature_policy synthetic_policy() { return {supported_os_feature, synthetic_control_feature}; }
} // namespace

TEST_CASE("PCI _OSC validation requires complete matching query and commit evidence", "[pci][osc]")
{
    const auto scope = make_scope();
    const auto evidence = make_evidence(scope);
    CHECK(pci::validate_root_feature_evidence(evidence, scope, synthetic_policy()) == pci::root_feature_error::none);

    auto stale = evidence;
    stale.platform_topology_generation++;
    CHECK(pci::validate_root_feature_evidence(stale, scope, synthetic_policy()) ==
          pci::root_feature_error::stale_scope);

    auto root_mismatch = evidence;
    root_mismatch.segment++;
    CHECK(pci::validate_root_feature_evidence(root_mismatch, scope, synthetic_policy()) ==
          pci::root_feature_error::root_mismatch);

    auto profile_mismatch = evidence;
    profile_mismatch.compatibility_profile_hash++;
    CHECK(pci::validate_root_feature_evidence(profile_mismatch, scope, synthetic_policy()) ==
          pci::root_feature_error::stale_scope);
}

TEST_CASE("bound PCI root scope seals platform generation and compatibility profile", "[pci][osc]")
{
    const auto scope = pci::bind_root_feature_scope(0x1122, 7, 3, 5, 9, 11, 13, 17, 32, 63);
    auto evidence = make_evidence(scope);

    CHECK(pci::validate_root_feature_evidence(evidence, scope, synthetic_policy()) == pci::root_feature_error::none);

    evidence.platform_topology_generation++;
    CHECK(pci::validate_root_feature_evidence(evidence, scope, synthetic_policy()) ==
          pci::root_feature_error::stale_scope);
    evidence = make_evidence(scope);
    evidence.compatibility_profile_hash++;
    CHECK(pci::validate_root_feature_evidence(evidence, scope, synthetic_policy()) ==
          pci::root_feature_error::stale_scope);
}

TEST_CASE("production zero-control _OSC policy validates without granting authority", "[pci][osc]")
{
    const auto scope = make_scope();
    auto evidence = make_evidence(scope);
    evidence.requested_control = 0;
    evidence.query_granted_control = 0;
    evidence.masked_control = 0;
    evidence.granted_control = 0;
    evidence.denied_control = 0;
    const pci::root_feature_policy production_policy{pci::implemented_pci_osc_support_mask,
                                                     pci::implemented_pci_osc_control_mask};
    CHECK(pci::implemented_pci_osc_control_mask == 0);
    CHECK(pci::validate_root_feature_evidence(evidence, scope, production_policy) ==
          pci::root_feature_error::no_control_granted);
}

TEST_CASE("production _OSC policy rejects every unimplemented control request", "[pci][osc]")
{
    const auto scope = make_scope();
    const auto evidence = make_evidence(scope);
    const pci::root_feature_policy production_policy{pci::implemented_pci_osc_support_mask,
                                                     pci::implemented_pci_osc_control_mask};
    CHECK(pci::implemented_pci_osc_control_mask == 0);
    CHECK(pci::validate_root_feature_evidence(evidence, scope, production_policy) ==
          pci::root_feature_error::unsupported_mask);
}

TEST_CASE("Denied, failed, or incomplete _OSC evidence is rejected", "[pci][osc]")
{
    const auto scope = make_scope();
    const auto evidence = make_evidence(scope);

    auto denied = evidence;
    denied.granted_control = 0;
    denied.denied_control = synthetic_control_feature;
    CHECK(pci::validate_root_feature_evidence(denied, scope, synthetic_policy()) ==
          pci::root_feature_error::no_control_granted);

    auto query_failed = evidence;
    query_failed.query_errors = 1;
    CHECK(pci::validate_root_feature_evidence(query_failed, scope, synthetic_policy()) ==
          pci::root_feature_error::query_failed);

    auto commit_failed = evidence;
    commit_failed.commit_errors = 1;
    CHECK(pci::validate_root_feature_evidence(commit_failed, scope, synthetic_policy()) ==
          pci::root_feature_error::commit_failed);

    auto query_incomplete = evidence;
    query_incomplete.query_support = 0;
    CHECK(pci::validate_root_feature_evidence(query_incomplete, scope, synthetic_policy()) ==
          pci::root_feature_error::incomplete_negotiation);

    auto commit_incomplete = evidence;
    commit_incomplete.commit_support = 0;
    CHECK(pci::validate_root_feature_evidence(commit_incomplete, scope, synthetic_policy()) ==
          pci::root_feature_error::incomplete_negotiation);
}

TEST_CASE("PCI root feature evidence cannot widen requested or implemented masks", "[pci][osc]")
{
    const auto scope = make_scope();
    const auto evidence = make_evidence(scope);

    auto unrequested_grant = evidence;
    unrequested_grant.granted_control |= 1u << 7;
    CHECK(pci::validate_root_feature_evidence(unrequested_grant, scope, synthetic_policy()) ==
          pci::root_feature_error::inconsistent_result);

    auto inconsistent_denial = evidence;
    inconsistent_denial.denied_control = synthetic_control_feature;
    CHECK(pci::validate_root_feature_evidence(inconsistent_denial, scope, synthetic_policy()) ==
          pci::root_feature_error::inconsistent_result);

    auto unknown_support = evidence;
    unknown_support.requested_support |= 1u << 20;
    CHECK(pci::validate_root_feature_evidence(unknown_support, scope, synthetic_policy()) ==
          pci::root_feature_error::unsupported_mask);

    auto unknown_control = evidence;
    unknown_control.requested_control |= 1u << 20;
    CHECK(pci::validate_root_feature_evidence(unknown_control, scope, synthetic_policy()) ==
          pci::root_feature_error::unsupported_mask);

    auto inconsistent_query_mask = evidence;
    inconsistent_query_mask.masked_control = synthetic_control_feature;
    CHECK(pci::validate_root_feature_evidence(inconsistent_query_mask, scope, synthetic_policy()) ==
          pci::root_feature_error::inconsistent_result);
}
