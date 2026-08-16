#include "catch2_compat.hpp"

#include "kernel/ipc/bounded_queue.hpp"
#include "kernel/usercopy.hpp"
#include "naos/abi.h"
#include "naos/libnao.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace
{
void record_task(void *context)
{
    auto *value = static_cast<int *>(context);
    *value = *value * 10 + 1;
}

void test_libnao_queues()
{
    nao::task_queue<2> tasks;
    int first = 0;
    int second = 0;
    REQUIRE(tasks.push(record_task, &first));
    REQUIRE(tasks.push(record_task, &second));
    REQUIRE(!tasks.push(record_task, &first));
    REQUIRE(tasks.run_one());
    REQUIRE(tasks.run_one());
    REQUIRE(!tasks.run_one());
    REQUIRE(first == 1);
    REQUIRE(second == 1);

    nao::timer_queue<2> timers;
    REQUIRE(timers.arm(7, 300));
    REQUIRE(timers.arm(11, 100));
    REQUIRE(!timers.arm(13, 500));
    std::uint64_t deadline = 0;
    REQUIRE(timers.next_deadline(deadline));
    REQUIRE(deadline == 100);
    std::uint64_t expired = 0;
    REQUIRE(timers.pop_expired(150, expired));
    REQUIRE(expired == 11);
    REQUIRE(timers.cancel(7));
    REQUIRE(!timers.next_deadline(deadline));
}

void test_fifo_and_capacity()
{
    std::array<int, 2> storage{};
    naos::ipc::bounded_queue<int> queue(storage.data(), storage.size());

    REQUIRE(queue.try_push(7));
    REQUIRE(queue.try_push(11));
    REQUIRE(!queue.try_push(13));
    REQUIRE(queue.size() == 2);

    REQUIRE(queue.claim_front());
    REQUIRE(queue.front() == 7);
    REQUIRE(!queue.claim_front());
    queue.cancel_claim();

    REQUIRE(queue.claim_front());
    REQUIRE(queue.front() == 7);
    queue.commit_claim();
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.front() == 11);

    REQUIRE(queue.claim_front());
    queue.commit_claim();
    REQUIRE(queue.empty());
}

void test_public_layout()
{
    static_assert(sizeof(na_handle_t) == sizeof(std::uint64_t));
    static_assert(NA_HANDLE_INVALID == 0);
    static_assert(offsetof(na_resource_disposition_t, handle) == 0);
    static_assert(offsetof(na_resource_disposition_t, operation) == 8);
    static_assert(offsetof(na_resource_disposition_t, rights) == 16);
    static_assert(sizeof(na_resource_disposition_t) == 32);
}

void test_remove_unclaimed_entry()
{
    std::array<int *, 3> storage{};
    naos::ipc::bounded_queue<int *> queue(storage.data(), storage.size());
    int first = 1;
    int second = 2;
    int third = 3;

    REQUIRE(queue.try_push(&first));
    REQUIRE(queue.try_push(&second));
    REQUIRE(queue.try_push(&third));
    REQUIRE(queue.remove(&second));
    REQUIRE(queue.size() == 2);
    REQUIRE(queue.at(0) == &first);
    REQUIRE(queue.at(1) == &third);
    REQUIRE(!queue.remove(&second));

    REQUIRE(queue.claim_front());
    REQUIRE(!queue.remove(&first));
    REQUIRE(queue.remove(&third));
    REQUIRE(queue.size() == 1);
    queue.cancel_claim();
}

void test_zero_length_ranges_require_null()
{
    int value = 0;
    REQUIRE(naos::usercopy::valid_range(0, 0));
    REQUIRE(!naos::usercopy::valid_range(reinterpret_cast<std::uint64_t>(&value), 0));

    REQUIRE(naos::usercopy::valid_output_range(reinterpret_cast<std::uint64_t>(&value), sizeof(value)));
    REQUIRE(naos::usercopy::valid_output_range(0, 0));
    REQUIRE(!naos::usercopy::valid_output_range(reinterpret_cast<std::uint64_t>(&value), 0));
}
} // namespace

TEST_CASE("phase 1 channel and queue contracts", "[ipc][phase1]")
{
    test_libnao_queues();
    test_fifo_and_capacity();
    test_public_layout();
    test_remove_unclaimed_entry();
    test_zero_length_ranges_require_null();
}
