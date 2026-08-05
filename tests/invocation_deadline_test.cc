#include "kernel/ipc/invocation_deadline.hpp"
#include <cassert>

int main()
{
    using naos::ipc::deadline_action;
    using naos::ipc::deadline_phase;

    assert(naos::ipc::deadline_action_for(deadline_phase::queued, false) == deadline_action::none);
    assert(naos::ipc::deadline_action_for(deadline_phase::queued, true) == deadline_action::remove_queued);
    assert(naos::ipc::deadline_action_for(deadline_phase::receiving, true) == deadline_action::outcome_unknown);
    assert(naos::ipc::deadline_action_for(deadline_phase::dispatched, true) == deadline_action::outcome_unknown);
    assert(naos::ipc::deadline_action_for(deadline_phase::ready, true) == deadline_action::none);
    assert(naos::ipc::deadline_action_for(deadline_phase::consumed, true) == deadline_action::none);
    return 0;
}
