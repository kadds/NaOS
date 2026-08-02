#include "kernel/ipc/bounded_queue.hpp"
#include "naos/object_call.h"

#include <array>
#include <cassert>
#include <cstddef>

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
} // namespace

int main()
{
    test_fifo_and_capacity();
    test_public_layout();
    return 0;
}
