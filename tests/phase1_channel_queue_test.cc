#include "kernel/ipc/bounded_queue.hpp"
#include "kernel/usercopy.hpp"
#include "naos/abi.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace
{
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

int main()
{
    test_fifo_and_capacity();
    test_public_layout();
    test_remove_unclaimed_entry();
    test_zero_length_ranges_require_null();
    return 0;
}
