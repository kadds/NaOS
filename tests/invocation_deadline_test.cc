#include "catch2_compat.hpp"
#include "kernel/ipc/invocation_deadline.hpp"

TEST_CASE("invocation deadline actions", "[invocation][deadline]")
{
    using naos::ipc::deadline_action;
    using naos::ipc::deadline_phase;

    REQUIRE(naos::ipc::deadline_action_for(deadline_phase::queued, false) == deadline_action::none);
    REQUIRE(naos::ipc::deadline_action_for(deadline_phase::queued, true) == deadline_action::remove_queued);
    REQUIRE(naos::ipc::deadline_action_for(deadline_phase::receiving, true) == deadline_action::outcome_unknown);
    REQUIRE(naos::ipc::deadline_action_for(deadline_phase::dispatched, true) == deadline_action::outcome_unknown);
    REQUIRE(naos::ipc::deadline_action_for(deadline_phase::ready, true) == deadline_action::none);
    REQUIRE(naos::ipc::deadline_action_for(deadline_phase::consumed, true) == deadline_action::none);
}
