#pragma once

#include "kernel/types.hpp"

namespace naos::ipc
{

enum class deadline_phase : u8
{
    queued,
    receiving,
    dispatched,
    ready,
    consumed,
};

enum class deadline_action : u8
{
    none,
    remove_queued,
    outcome_unknown,
};

constexpr deadline_action deadline_action_for(deadline_phase phase, bool expired)
{
    if (!expired || phase == deadline_phase::ready || phase == deadline_phase::consumed)
        return deadline_action::none;
    return phase == deadline_phase::queued ? deadline_action::remove_queued : deadline_action::outcome_unknown;
}

} // namespace naos::ipc
