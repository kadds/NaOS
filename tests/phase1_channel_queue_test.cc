#include "kernel/ipc/bounded_queue.hpp"
#include "kernel/usercopy.hpp"
#include "naos/abi.h"
#include "naos/libnao.hpp"

#include <array>
#include <cassert>
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
    assert(tasks.push(record_task, &first));
    assert(tasks.push(record_task, &second));
    assert(!tasks.push(record_task, &first));
    assert(tasks.run_one());
    assert(tasks.run_one());
    assert(!tasks.run_one());
    assert(first == 1);
    assert(second == 1);

    nao::timer_queue<2> timers;
    assert(timers.arm(7, 300));
    assert(timers.arm(11, 100));
    assert(!timers.arm(13, 500));
    std::uint64_t deadline = 0;
    assert(timers.next_deadline(deadline));
    assert(deadline == 100);
    std::uint64_t expired = 0;
    assert(timers.pop_expired(150, expired));
    assert(expired == 11);
    assert(timers.cancel(7));
    assert(!timers.next_deadline(deadline));
}

void test_fifo_and_capacity()
{
    std::array<int, 2> storage{};
    naos::ipc::bounded_queue<int> queue(storage.data(), storage.size());

    assert(queue.try_push(7));
    assert(queue.try_push(11));
    assert(!queue.try_push(13));
    assert(queue.size() == 2);

    assert(queue.claim_front());
    assert(queue.front() == 7);
    assert(!queue.claim_front());
    queue.cancel_claim();

    assert(queue.claim_front());
    assert(queue.front() == 7);
    queue.commit_claim();
    assert(queue.size() == 1);
    assert(queue.front() == 11);

    assert(queue.claim_front());
    queue.commit_claim();
    assert(queue.empty());
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

    assert(queue.try_push(&first));
    assert(queue.try_push(&second));
    assert(queue.try_push(&third));
    assert(queue.remove(&second));
    assert(queue.size() == 2);
    assert(queue.at(0) == &first);
    assert(queue.at(1) == &third);
    assert(!queue.remove(&second));

    assert(queue.claim_front());
    assert(!queue.remove(&first));
    assert(queue.remove(&third));
    assert(queue.size() == 1);
    queue.cancel_claim();
}

void test_zero_length_ranges_require_null()
{
    int value = 0;
    assert(naos::usercopy::valid_range(0, 0));
    assert(!naos::usercopy::valid_range(reinterpret_cast<std::uint64_t>(&value), 0));

    assert(naos::usercopy::valid_output_range(reinterpret_cast<std::uint64_t>(&value), sizeof(value)));
    assert(naos::usercopy::valid_output_range(0, 0));
    assert(!naos::usercopy::valid_output_range(reinterpret_cast<std::uint64_t>(&value), 0));
}
} // namespace

int run_phase1_channel_queue_tests()
{
    test_libnao_queues();
    test_fifo_and_capacity();
    test_public_layout();
    test_remove_unclaimed_entry();
    test_zero_length_ranges_require_null();
    return 0;
}
