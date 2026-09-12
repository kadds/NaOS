#include <naos/ipc_core.h>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
void *allocate(void *, std::size_t size, std::size_t alignment)
{
    alignment = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    void *pointer = nullptr;
    return ::posix_memalign(&pointer, alignment, size) == 0 ? pointer : nullptr;
}

void deallocate(void *, void *pointer, std::size_t, std::size_t) { std::free(pointer); }

void lock_acquire(void *context)
{
    auto &lock = *static_cast<std::atomic_flag *>(context);
    while (lock.test_and_set(std::memory_order_acquire))
        std::this_thread::yield();
}

void lock_release(void *context) { static_cast<std::atomic_flag *>(context)->clear(std::memory_order_release); }

void notify(void *context) { static_cast<std::atomic_uint *>(context)->fetch_add(1, std::memory_order_relaxed); }

std::uint64_t clock_now(void *context)
{
    return static_cast<std::atomic_uint64_t *>(context)->load(std::memory_order_relaxed);
}

struct CallbackState
{
    std::atomic_uint removed{0};
    std::atomic_uint woken{0};
};

int remove_queued(void *context)
{
    static_cast<CallbackState *>(context)->removed.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

void wake_execution(void *context)
{
    static_cast<CallbackState *>(context)->woken.fetch_add(1, std::memory_order_relaxed);
}

void release_resource(void *context, void *value)
{
    static_cast<std::atomic_uint *>(context)->fetch_add(1, std::memory_order_relaxed);
    std::free(value);
}

struct Fixture
{
    std::atomic_flag domain_lock = ATOMIC_FLAG_INIT;
    std::atomic_flag invocation_lock = ATOMIC_FLAG_INIT;
    std::atomic_uint notifications{0};
    std::atomic_uint64_t clock{0};
    CallbackState callbacks;
    naos_ipc_allocator_t allocator{nullptr, allocate, deallocate};
    naos_ipc_lock_t domain_lock_api{&domain_lock, lock_acquire, lock_release};
    naos_ipc_lock_t invocation_lock_api{&invocation_lock, lock_acquire, lock_release};
    naos_ipc_wait_notifier_t notifier{&notifications, notify};
    naos_ipc_clock_t clock_api{&clock, clock_now};
    naos_ipc_invocation_callbacks_t invocation_callbacks{&callbacks, remove_queued, wake_execution};
    naos_ipc_domain_t *domain = nullptr;

    Fixture()
    {
        naos_ipc_domain_config_t config{&allocator, &domain_lock_api, 64, 4096, 64};
        domain = naos_ipc_domain_create(&config);
        assert(domain != nullptr);
    }

    ~Fixture() { naos_ipc_domain_destroy(domain); }

    naos_ipc_invocation_t *create(std::uint64_t deadline = 0)
    {
        naos_ipc_invocation_config_t config{
            domain,   &allocator, &allocator, &invocation_lock_api, &notifier, &clock_api, &invocation_callbacks, 77,
            deadline, 128,        4,
        };
        auto *invocation = naos_ipc_invocation_create(&config);
        assert(invocation != nullptr);
        assert(naos_ipc_invocation_method_id(invocation) == 77);
        assert(naos_ipc_invocation_operation_deadline(invocation) == deadline);
        assert(naos_ipc_invocation_reserve_result_budget(invocation) != 0);
        return invocation;
    }
};

void dispatch(naos_ipc_invocation_t *invocation)
{
    assert(naos_ipc_invocation_begin_receive(invocation) != 0);
    assert(naos_ipc_invocation_finish_dispatch(invocation) != 0);
}

void claim_and_commit_empty(naos_ipc_invocation_t *invocation, std::uint32_t expected_outcome,
                            std::uint32_t expected_reason)
{
    naos_ipc_invocation_result_info_t info{};
    assert(naos_ipc_invocation_claim_result(invocation, 0, 0, &info) == NAOS_IPC_STATUS_OK);
    assert(info.actual_bytes == 0);
    assert(info.actual_resources == 0);
    assert(info.execution_outcome == expected_outcome);
    assert(info.outcome_reason == expected_reason);
    assert(naos_ipc_invocation_commit_result(invocation) == NAOS_IPC_STATUS_OK);
    assert((naos_ipc_invocation_signals(invocation) & NAOS_IPC_SIGNAL_COMPLETED) != 0);
}

void test_reply_ownership_and_capacity()
{
    Fixture fixture;
    auto *invocation = fixture.create();
    dispatch(invocation);

    std::atomic_uint released{0};
    auto *token = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    assert(token != nullptr);
    *token = 1234;
    naos_ipc_resource_t resource{&released, token, release_resource};
    constexpr char response[] = "hello";
    assert(naos_ipc_invocation_complete_reply(invocation, reinterpret_cast<const std::uint8_t *>(response), 5,
                                              &resource, 1, 9) != 0);
    assert(!naos_ipc_resource_valid(&resource));
    assert((naos_ipc_invocation_signals(invocation) & NAOS_IPC_SIGNAL_COMPLETED) != 0);

    naos_ipc_invocation_result_info_t info{};
    assert(naos_ipc_invocation_claim_result(invocation, 2, 1, &info) == NAOS_IPC_STATUS_BUFFER_TOO_SMALL);
    assert(info.actual_bytes == 5);
    assert(info.actual_resources == 1);
    assert(info.required_bytes == 5);
    assert(info.required_resources == 1);
    assert(naos_ipc_invocation_claim_result(invocation, 5, 1, &info) == NAOS_IPC_STATUS_OK);
    assert(info.protocol_error == -9);

    std::uint8_t *bytes = nullptr;
    std::uint64_t byte_count = 0;
    assert(naos_ipc_invocation_take_result_bytes(invocation, &bytes, &byte_count) == NAOS_IPC_STATUS_OK);
    assert(byte_count == 5);
    assert(std::memcmp(bytes, response, byte_count) == 0);
    naos_ipc_resource_t received{};
    assert(naos_ipc_invocation_take_result_resource(invocation, 0, &received) == NAOS_IPC_STATUS_OK);
    assert(*static_cast<std::uint64_t *>(received.value) == 1234);
    naos_ipc_resource_reset(&received);
    assert(naos_ipc_invocation_commit_result(invocation) == NAOS_IPC_STATUS_OK);
    assert(released == 1);
    assert(naos_ipc_invocation_claim_result(invocation, 0, 0, &info) == NAOS_IPC_STATUS_ALREADY_CONSUMED);
    naos_ipc_invocation_destroy(invocation);
}

void test_queued_cancel_is_not_delivered()
{
    Fixture fixture;
    auto *invocation = fixture.create();
    assert(naos_ipc_invocation_cancel(invocation) != 0);
    assert(fixture.callbacks.removed == 1);
    assert((naos_ipc_invocation_signals(invocation) & NAOS_IPC_SIGNAL_CANCEL_REQUESTED) != 0);
    claim_and_commit_empty(invocation, NAOS_IPC_EXECUTION_NOT_DELIVERED, NAOS_IPC_OUTCOME_REASON_CANCEL_REQUESTED);
    assert(fixture.callbacks.woken == 0);
    naos_ipc_invocation_destroy(invocation);
}

void test_dispatched_cancel_interrupts_until_failure()
{
    Fixture fixture;
    auto *invocation = fixture.create();
    dispatch(invocation);
    assert(naos_ipc_invocation_cancel(invocation) != 0);
    assert(naos_ipc_invocation_cancellation_requested(invocation) != 0);
    assert(naos_ipc_invocation_execution_interrupted(invocation) != 0);
    naos_ipc_invocation_result_info_t info{};
    assert(naos_ipc_invocation_claim_result(invocation, 0, 0, &info) == NAOS_IPC_STATUS_WOULD_BLOCK);
    assert(naos_ipc_invocation_complete_failure(invocation, NAOS_IPC_EXECUTION_OUTCOME_UNKNOWN,
                                                NAOS_IPC_OUTCOME_REASON_CANCEL_REQUESTED, 0) != 0);
    claim_and_commit_empty(invocation, NAOS_IPC_EXECUTION_OUTCOME_UNKNOWN, NAOS_IPC_OUTCOME_REASON_CANCEL_REQUESTED);
    assert(fixture.callbacks.woken != 0);
    naos_ipc_invocation_destroy(invocation);
}

void test_responder_close_while_receiving_is_terminal()
{
    Fixture fixture;
    auto *invocation = fixture.create();
    assert(naos_ipc_invocation_begin_receive(invocation) != 0);
    naos_ipc_invocation_abandon_responder(invocation);

    // The responder may disappear after the receiver claims the request but
    // before delivery is committed.  finish_dispatch must publish a
    // terminal result instead of transitioning to an unanswerable dispatch.
    assert(naos_ipc_invocation_finish_dispatch(invocation) == 0);
    claim_and_commit_empty(invocation, NAOS_IPC_EXECUTION_OUTCOME_UNKNOWN, NAOS_IPC_OUTCOME_REASON_RESPONDER_ABANDONED);
    naos_ipc_invocation_destroy(invocation);
}

void test_deadline_publishes_not_delivered()
{
    Fixture fixture;
    fixture.clock.store(100, std::memory_order_relaxed);
    auto *invocation = fixture.create(100);
    assert(naos_ipc_invocation_expire_if_due(invocation) != 0);
    assert(fixture.callbacks.removed == 1);
    claim_and_commit_empty(invocation, NAOS_IPC_EXECUTION_NOT_DELIVERED, NAOS_IPC_OUTCOME_REASON_OPERATION_DEADLINE);
    naos_ipc_invocation_destroy(invocation);
}
} // namespace

int main()
{
    test_reply_ownership_and_capacity();
    test_queued_cancel_is_not_delivered();
    test_dispatched_cancel_interrupts_until_failure();
    test_responder_close_while_receiving_is_terminal();
    test_deadline_publishes_not_delivered();
}
