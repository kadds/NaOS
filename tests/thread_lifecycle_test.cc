#include "catch2_compat.hpp"
#include "kernel/task/thread_lifecycle.hpp"
#include <barrier>
#include <thread>

TEST_CASE("a failed exit claim can be retried", "[task][lifecycle]")
{
    task::thread_lifecycle lifecycle;
    REQUIRE(lifecycle.try_begin_exit());
    REQUIRE(lifecycle.started());
    REQUIRE_FALSE(lifecycle.try_begin_exit());
    REQUIRE(lifecycle.cancel_exit());
    REQUIRE_FALSE(lifecycle.started());
    REQUIRE(lifecycle.try_begin_exit());
}

TEST_CASE("exit publication precedes exclusive reaping", "[task][lifecycle]")
{
    task::thread_lifecycle lifecycle;
    REQUIRE_FALSE(lifecycle.try_begin_reap());
    REQUIRE(lifecycle.try_begin_exit());
    REQUIRE_FALSE(lifecycle.can_reap());
    REQUIRE_FALSE(lifecycle.try_begin_reap());
    REQUIRE(lifecycle.complete_exit());
    REQUIRE(lifecycle.can_reap());
    REQUIRE_FALSE(lifecycle.cancel_exit());
    REQUIRE_FALSE(lifecycle.try_begin_exit());
    REQUIRE(lifecycle.try_begin_reap());
    REQUIRE(lifecycle.is_reaping());
    REQUIRE_FALSE(lifecycle.try_begin_reap());
}

TEST_CASE("an unpublished thread can bypass scheduler exit", "[task][lifecycle]")
{
    task::thread_lifecycle lifecycle;
    REQUIRE(lifecycle.discard_unstarted());
    REQUIRE(lifecycle.can_reap());
    REQUIRE_FALSE(lifecycle.try_begin_exit());
    REQUIRE(lifecycle.try_begin_reap());
}

TEST_CASE("concurrent reapers claim a thread exactly once", "[task][lifecycle]")
{
    for (int iteration = 0; iteration < 32; ++iteration)
    {
        task::thread_lifecycle lifecycle;
        REQUIRE(lifecycle.try_begin_exit());
        REQUIRE(lifecycle.complete_exit());
        std::barrier gate(3);
        std::atomic_uint winners{0};
        auto reap = [&] {
            gate.arrive_and_wait();
            if (lifecycle.try_begin_reap())
                winners.fetch_add(1);
        };
        std::thread first(reap);
        std::thread second(reap);
        gate.arrive_and_wait();
        first.join();
        second.join();
        REQUIRE(winners.load() == 1);
    }
}
