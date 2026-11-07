#pragma once

#include "kernel/types.hpp"
#include <atomic>

namespace task
{
enum class process_start_state : u8
{
    unstarted,
    started,
    aborted,
};

enum class process_capability_release_action : u8
{
    none,
    abort_unstarted,
    finalize_reaped,
};

inline bool claim_process_start(std::atomic<process_start_state> &state) noexcept
{
    auto expected = process_start_state::unstarted;
    return state.compare_exchange_strong(expected, process_start_state::started, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

inline bool claim_unstarted_process_abort(std::atomic<process_start_state> &state) noexcept
{
    auto expected = process_start_state::unstarted;
    return state.compare_exchange_strong(expected, process_start_state::aborted, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

/// Decide whether dropping a process capability completes the lifetime of its
/// process object. Deferred children are canceled when their final handle is
/// dropped before start; started processes remain in the process table until
/// their normal exit/reap path completes.
constexpr process_capability_release_action process_capability_release_action_for(u32 previous_capability_refs,
                                                                                  process_start_state start_state,
                                                                                  bool reap_pending) noexcept
{
    if (previous_capability_refs != 1)
        return process_capability_release_action::none;
    if (reap_pending)
        return process_capability_release_action::finalize_reaped;
    if (start_state == process_start_state::unstarted)
        return process_capability_release_action::abort_unstarted;
    return process_capability_release_action::none;
}
} // namespace task
