#include <catch2/catch_test_macros.hpp>

#include "kernel/irq/ipi_drain.hpp"
#include "kernel/irq/soft_irq_drain.hpp"
#include "kernel/timer_work_budget.hpp"

TEST_CASE("soft IRQ drain revisits a vector re-raised during its handler", "[soft_irq]")
{
    bool pending = true;
    u32 invocations = 0;

    irq::detail::drain_requeued_soft_interrupts(
        2, 8,
        [&](u32 vector) {
            if (vector != 1 || !pending)
                return false;
            pending = false;
            ++invocations;
            if (invocations == 1)
                pending = true;
            return true;
        },
        [&] { return pending; });

    CHECK(invocations == 2);
    CHECK_FALSE(pending);
}

TEST_CASE("soft IRQ drain yields after its bounded requeue limit", "[soft_irq]")
{
    bool pending = true;
    u32 invocations = 0;

    irq::detail::drain_requeued_soft_interrupts(
        1, 4,
        [&](u32) {
            ++invocations;
            pending = true;
            return true;
        },
        [&] { return pending; });

    CHECK(invocations == 4);
    CHECK(pending);
}

TEST_CASE("soft IRQ exit barrier drains work that arrived just before interrupts were disabled", "[soft_irq]")
{
    bool interrupts_enabled = true;
    bool pending = false;
    bool drained = false;

    irq::detail::drain_soft_interrupts_at_exit_barrier(
        [&] {
            // Model an IRQ arriving after the main drain returned but just
            // before the local IF-clear takes effect.
            pending = true;
            interrupts_enabled = false;
        },
        [&] { return pending; },
        [&] {
            CHECK_FALSE(interrupts_enabled);
            CHECK(pending);
            pending = false;
            drained = true;
        });

    CHECK(drained);
    CHECK_FALSE(pending);
}

TEST_CASE("IPI work drain yields after its bounded batch", "[ipi][irq]")
{
    const u32 operations[] = {3, 5, 7, 11, 13};
    u32 next = 0;
    u32 operation = 0;
    u32 sum = 0;

    const bool budget_exhausted = irq::detail::drain_bounded_interrupt_work(
        3, operation,
        [&](u32 &dequeued) {
            if (next == 5)
                return false;
            dequeued = operations[next++];
            return true;
        },
        [&](u32 dequeued) { sum += dequeued; });

    CHECK(budget_exhausted);
    CHECK(next == 3);
    CHECK(sum == 15);
}

TEST_CASE("IPI work drain reports when the queue empties before the budget", "[ipi][irq]")
{
    const u32 operations[] = {2, 4};
    u32 next = 0;
    u32 operation = 0;
    u32 sum = 0;

    const bool budget_exhausted = irq::detail::drain_bounded_interrupt_work(
        4, operation,
        [&](u32 &dequeued) {
            if (next == 2)
                return false;
            dequeued = operations[next++];
            return true;
        },
        [&](u32 dequeued) { sum += dequeued; });

    CHECK_FALSE(budget_exhausted);
    CHECK(next == 2);
    CHECK(sum == 6);
}

TEST_CASE("timer due-work drain yields when its callback budget is consumed", "[timer]")
{
    u32 callbacks = 0;
    u32 continuations = 0;
    const auto result = timer::detail::drain_bounded_due_work(
        3,
        [&] {
            ++callbacks;
            return true;
        },
        [&] { ++continuations; });

    CHECK(result == timer::detail::due_work_result::continuation_required);
    CHECK(callbacks == 3);
    CHECK(continuations == 1);
}

TEST_CASE("timer due-work drain stops once the next watcher is not due", "[timer]")
{
    u32 callbacks = 0;
    u32 continuations = 0;
    const auto result = timer::detail::drain_bounded_due_work(
        4,
        [&] {
            ++callbacks;
            return callbacks < 3;
        },
        [&] { ++continuations; });

    CHECK(result == timer::detail::due_work_result::drained);
    CHECK(callbacks == 3);
    CHECK(continuations == 0);
}
