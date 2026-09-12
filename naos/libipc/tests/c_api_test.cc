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

naos_ipc_status_t validate_waitable(void *, std::uint64_t handle)
{
    return handle == 0 ? NAOS_IPC_STATUS_INVALID_HANDLE : NAOS_IPC_STATUS_OK;
}

void release_resource(void *context, void *value)
{
    static_cast<std::atomic_uint *>(context)->fetch_add(1, std::memory_order_relaxed);
    std::free(value);
}

naos_ipc_allocator_t allocator_api{nullptr, allocate, deallocate};
} // namespace

int main()
{
    std::atomic_flag domain_lock = ATOMIC_FLAG_INIT;
    std::atomic_flag channel_lock = ATOMIC_FLAG_INIT;
    std::atomic_uint notifications{0};
    naos_ipc_lock_t domain_lock_api{&domain_lock, lock_acquire, lock_release};
    naos_ipc_lock_t channel_lock_api{&channel_lock, lock_acquire, lock_release};
    naos_ipc_wait_notifier_t notifier{&notifications, notify};
    naos_ipc_handle_table_t handles{nullptr, validate_waitable, nullptr, nullptr, nullptr};

    naos_ipc_domain_config_t domain_config{&allocator_api, &domain_lock_api, 128, 4096, 128};
    auto *domain = naos_ipc_domain_create(&domain_config);
    assert(domain != nullptr);
    naos_ipc_channel_config_t channel_config{
        domain, &allocator_api, &allocator_api, &channel_lock_api, &notifier, nullptr, &handles, 2, 128, 4};
    auto *channel = naos_ipc_channel_create(&channel_config);
    assert(channel != nullptr);
    naos_ipc_channel_side_reference_acquired(channel, 0);
    naos_ipc_channel_side_reference_acquired(channel, 1);

    auto *message = naos_ipc_message_create(channel, 4, 1);
    assert(message != nullptr);
    std::memcpy(naos_ipc_message_bytes(message), "test", 4);
    auto *token = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    assert(token != nullptr);
    *token = 42;
    naos_ipc_resource_t resource{nullptr, token, nullptr};
    std::atomic_uint released{0};
    resource.context = &released;
    resource.release = release_resource;
    assert(naos_ipc_channel_enqueue(channel, 0, message, &resource, 1, 0) == NAOS_IPC_STATUS_OK);
    assert(!naos_ipc_resource_valid(&resource));

    naos_ipc_message_t *received = nullptr;
    assert(naos_ipc_channel_claim_receive(channel, 1, &received) == NAOS_IPC_STATUS_OK);
    assert(received != nullptr);
    assert(naos_ipc_message_byte_count(received) == 4);
    naos_ipc_resource_t received_resource{};
    assert(naos_ipc_message_take_resource(received, 0, &received_resource) == NAOS_IPC_STATUS_OK);
    assert(*static_cast<std::uint64_t *>(received_resource.value) == 42);
    naos_ipc_resource_reset(&received_resource);
    assert(naos_ipc_channel_commit_receive(channel, 1, received) != 0);
    naos_ipc_message_destroy(received);
    assert(released == 1);

    auto *first = naos_ipc_message_create(channel, 1, 0);
    auto *second = naos_ipc_message_create(channel, 1, 0);
    auto *third = naos_ipc_message_create(channel, 1, 0);
    assert(first != nullptr && second != nullptr && third != nullptr);
    assert(naos_ipc_channel_enqueue(channel, 0, first, nullptr, 0, 0) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_channel_enqueue(channel, 0, second, nullptr, 0, 0) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_channel_enqueue(channel, 0, third, nullptr, 0, 0) == NAOS_IPC_STATUS_WOULD_BLOCK);
    naos_ipc_message_destroy(third);
    assert(naos_ipc_channel_claim_receive(channel, 1, &received) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_channel_cancel_receive(channel, 1, received) != 0);
    assert(naos_ipc_channel_claim_receive(channel, 1, &received) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_channel_commit_receive(channel, 1, received) != 0);
    naos_ipc_message_destroy(received);
    assert(naos_ipc_channel_claim_receive(channel, 1, &received) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_channel_commit_receive(channel, 1, received) != 0);
    naos_ipc_message_destroy(received);

    constexpr unsigned concurrent_messages = 32;
    std::atomic_uint received_count{0};
    std::thread producer([&] {
        for (unsigned i = 0; i < concurrent_messages; i++)
        {
            auto *value = naos_ipc_message_create(channel, 0, 0);
            assert(value != nullptr);
            while (naos_ipc_channel_enqueue(channel, 0, value, nullptr, 0, 0) == NAOS_IPC_STATUS_WOULD_BLOCK)
                std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        while (received_count.load(std::memory_order_relaxed) != concurrent_messages)
        {
            naos_ipc_message_t *value = nullptr;
            if (naos_ipc_channel_claim_receive(channel, 1, &value) != NAOS_IPC_STATUS_OK)
            {
                std::this_thread::yield();
                continue;
            }
            assert(naos_ipc_channel_commit_receive(channel, 1, value) != 0);
            naos_ipc_message_destroy(value);
            received_count.fetch_add(1, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();

    const std::uint64_t wait_values[] = {1, 2};
    assert(naos_ipc_validate_wait_set(&handles, wait_values, 2) == NAOS_IPC_STATUS_OK);
    const std::uint64_t invalid_wait_values[] = {0};
    assert(naos_ipc_validate_wait_set(&handles, invalid_wait_values, 1) == NAOS_IPC_STATUS_INVALID_HANDLE);
    naos_ipc_channel_side_reference_released(channel, 0);
    assert((naos_ipc_channel_signals(channel, 1) & NAOS_IPC_SIGNAL_PEER_CLOSED) != 0);
    naos_ipc_channel_side_reference_released(channel, 1);
    naos_ipc_channel_destroy(channel);
    naos_ipc_domain_destroy(domain);
}
