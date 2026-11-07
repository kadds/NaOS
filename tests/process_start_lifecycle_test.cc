#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "kernel/task/process_start_lifecycle.hpp"

TEST_CASE("the last capability aborts a deferred process that never started", "[task][process]")
{
    CHECK(task::process_capability_release_action_for(1, task::process_start_state::unstarted, false) ==
          task::process_capability_release_action::abort_unstarted);
}

TEST_CASE("deferred process release waits while another capability remains", "[task][process]")
{
    CHECK(task::process_capability_release_action_for(2, task::process_start_state::unstarted, false) ==
          task::process_capability_release_action::none);
}

TEST_CASE("started process release does not abort a live process", "[task][process]")
{
    CHECK(task::process_capability_release_action_for(1, task::process_start_state::started, false) ==
          task::process_capability_release_action::none);
}

TEST_CASE("the final capability permits finalizing an already reaped process", "[task][process]")
{
    CHECK(task::process_capability_release_action_for(1, task::process_start_state::started, true) ==
          task::process_capability_release_action::finalize_reaped);
}

TEST_CASE("non-final capabilities do not finalize an already reaped process", "[task][process]")
{
    CHECK(task::process_capability_release_action_for(2, task::process_start_state::started, true) ==
          task::process_capability_release_action::none);
}

TEST_CASE("process start and unstarted abort claim one shared lifecycle state", "[task][process]")
{
    std::atomic<task::process_start_state> start_state{task::process_start_state::unstarted};

    CHECK(task::claim_process_start(start_state));
    CHECK_FALSE(task::claim_unstarted_process_abort(start_state));
    CHECK(start_state.load() == task::process_start_state::started);

    start_state.store(task::process_start_state::unstarted);
    CHECK(task::claim_unstarted_process_abort(start_state));
    CHECK_FALSE(task::claim_process_start(start_state));
    CHECK(start_state.load() == task::process_start_state::aborted);
}

TEST_CASE("concurrent start and abort have exactly one lifecycle winner", "[task][process]")
{
    for (int iteration = 0; iteration < 32; ++iteration)
    {
        std::atomic<task::process_start_state> start_state{task::process_start_state::unstarted};
        std::atomic_bool start_won{false};
        std::atomic_bool abort_won{false};
        std::barrier gate(3);

        std::thread starter([&] {
            gate.arrive_and_wait();
            start_won.store(task::claim_process_start(start_state), std::memory_order_relaxed);
        });
        std::thread aborter([&] {
            gate.arrive_and_wait();
            abort_won.store(task::claim_unstarted_process_abort(start_state), std::memory_order_relaxed);
        });
        gate.arrive_and_wait();
        starter.join();
        aborter.join();

        CHECK(start_won.load() != abort_won.load());
        CHECK(start_state.load() ==
              (start_won.load() ? task::process_start_state::started : task::process_start_state::aborted));
    }
}
