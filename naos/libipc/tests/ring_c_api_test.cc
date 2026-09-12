#include <naos/ipc_core.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

/*
 * Drives the ring exclusively through the stable C ABI in naos/ipc_core.h.
 * This is the route a Linux-side adapter takes: it owns the storage and the
 * platform callbacks and never sees the C++ channel/ring implementation.
 */
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

std::uint64_t clock_now(void *) { return 0; }

void release_resource(void *context, void *value)
{
    static_cast<std::atomic_uint *>(context)->fetch_add(1, std::memory_order_relaxed);
    std::free(value);
}

naos_ipc_allocator_t allocator_api{nullptr, allocate, deallocate};
} // namespace

int main()
{
    std::atomic_flag ring_lock = ATOMIC_FLAG_INIT;
    std::atomic_uint notifications{0};
    std::atomic_uint released{0};
    naos_ipc_lock_t lock_api{&ring_lock, lock_acquire, lock_release};
    naos_ipc_wait_notifier_t notifier{&notifications, notify};
    naos_ipc_clock_t clock_api{nullptr, clock_now};

    constexpr std::uint64_t slot_bytes = 16;
    constexpr std::uint32_t capacity = 4;
    constexpr std::uint32_t resources = 2;
    const std::uint64_t required = naos_ipc_ring_required_bytes(slot_bytes, capacity, resources);
    assert(required != 0);
    void *storage = nullptr;
    assert(::posix_memalign(&storage, 64, required) == 0);
    std::memset(storage, 0, required);

    assert(naos_ipc_ring_init(storage, required - 1, slot_bytes, capacity, resources) ==
           NAOS_IPC_STATUS_BUFFER_TOO_SMALL);
    // Unformatted storage cannot be attached.
    naos_ipc_ring_config_t config{&allocator_api, &lock_api,      &notifier,  &clock_api, nullptr,
                                  storage,          required,      slot_bytes, capacity,   resources};
    assert(naos_ipc_ring_create(&config) == nullptr);
    assert(naos_ipc_ring_init(storage, required, slot_bytes, capacity, resources) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_ring_init(storage, required, slot_bytes, capacity, resources) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_ring_init(storage, naos_ipc_ring_required_bytes(slot_bytes + 1, capacity, resources),
                              slot_bytes + 1, capacity, resources) == NAOS_IPC_STATUS_INVALID_ARGUMENT);

    auto *ring = naos_ipc_ring_create(&config);
    assert(ring != nullptr);
    assert(naos_ipc_ring_valid(ring) != 0);
    assert(naos_ipc_ring_slot_capacity(ring) == capacity);
    assert(naos_ipc_ring_slot_bytes(ring) == slot_bytes);
    assert(naos_ipc_ring_resource_capacity(ring) == resources);
    assert(naos_ipc_ring_queued(ring) == 0);
    assert(naos_ipc_ring_claim_pending(ring) == 0);
    assert((naos_ipc_ring_signals(ring, 0) & NAOS_IPC_SIGNAL_WRITABLE) != 0);
    assert((naos_ipc_ring_signals(ring, 1) & NAOS_IPC_SIGNAL_READABLE) == 0);

    std::uint8_t buffer[16] = {};
    std::uint64_t count = 99;
    std::uint64_t resource_count = 99;
    assert(naos_ipc_ring_claim(ring, buffer, sizeof buffer, &count, &resource_count) == NAOS_IPC_STATUS_WOULD_BLOCK);
    assert(count == 0 && resource_count == 0);
    assert(naos_ipc_ring_commit(ring) == NAOS_IPC_STATUS_INVALID_HANDLE);
    assert(naos_ipc_ring_take_resource(ring, 0, nullptr) == NAOS_IPC_STATUS_INVALID_ARGUMENT);

    auto *token = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    assert(token != nullptr);
    *token = 42;
    naos_ipc_resource_t resource{&released, token, release_resource};
    const std::uint8_t payload[3] = {'a', 'b', 'c'};
    assert(naos_ipc_ring_send(ring, payload, sizeof payload, &resource, 1) == NAOS_IPC_STATUS_OK);
    assert(!naos_ipc_resource_valid(&resource));
    assert(naos_ipc_ring_queued(ring) == 1);
    assert(naos_ipc_ring_claim(ring, buffer, sizeof buffer, &count, &resource_count) == NAOS_IPC_STATUS_OK);
    assert(count == 3 && resource_count == 1 && std::memcmp(buffer, payload, 3) == 0);
    assert(naos_ipc_ring_claim_pending(ring) != 0);
    naos_ipc_resource_t taken{};
    assert(naos_ipc_ring_take_resource(ring, 0, &taken) == NAOS_IPC_STATUS_OK);
    assert(*static_cast<std::uint64_t *>(taken.value) == 42);
    naos_ipc_resource_reset(&taken);
    assert(released.load() == 1);
    assert(naos_ipc_ring_commit(ring) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_ring_queued(ring) == 0);

    // Backpressure through the C ABI.
    for (std::uint32_t index = 0; index < capacity; index++)
        assert(naos_ipc_ring_send(ring, payload, 1, nullptr, 0) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_ring_send(ring, payload, 1, nullptr, 0) == NAOS_IPC_STATUS_WOULD_BLOCK);
    assert(naos_ipc_ring_queued(ring) == capacity);
    assert((naos_ipc_ring_signals(ring, 0) & NAOS_IPC_SIGNAL_WRITABLE) == 0);
    for (std::uint32_t index = 0; index < capacity; index++)
    {
        assert(naos_ipc_ring_claim(ring, buffer, sizeof buffer, &count, &resource_count) == NAOS_IPC_STATUS_OK);
        assert(naos_ipc_ring_commit(ring) == NAOS_IPC_STATUS_OK);
    }

    // Concurrent producer/consumer over the C ABI.
    constexpr unsigned total = 4000;
    std::atomic<unsigned> received{0};
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for (unsigned sequence = 0; sequence < total; sequence++)
        {
            const std::uint32_t value = sequence * 40503u;
            while (naos_ipc_ring_send(ring, reinterpret_cast<const std::uint8_t *>(&value), sizeof value, nullptr,
                                      0) == NAOS_IPC_STATUS_WOULD_BLOCK)
                std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        unsigned expected = 0;
        while (expected < total)
        {
            std::uint8_t local[16] = {};
            std::uint64_t bytes = 0;
            std::uint64_t resources_seen = 0;
            const auto status = naos_ipc_ring_claim(ring, local, sizeof local, &bytes, &resources_seen);
            if (status == NAOS_IPC_STATUS_WOULD_BLOCK)
            {
                std::this_thread::yield();
                continue;
            }
            std::uint32_t value = 0;
            std::memcpy(&value, local, sizeof value);
            if (status != NAOS_IPC_STATUS_OK || bytes != sizeof value || value != expected * 40503u)
            {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            if (naos_ipc_ring_commit(ring) != NAOS_IPC_STATUS_OK)
            {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            expected++;
            received.store(expected, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();
    assert(!failed.load());
    assert(received.load() == total);
    assert(naos_ipc_ring_queued(ring) == 0);

    // Required-size reporting and close semantics.
    const std::uint8_t long_message[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(naos_ipc_ring_send(ring, long_message, sizeof long_message, nullptr, 0) == NAOS_IPC_STATUS_OK);
    assert(naos_ipc_ring_claim(ring, buffer, 4, &count, &resource_count) == NAOS_IPC_STATUS_BUFFER_TOO_SMALL);
    assert(count == 8);
    assert(naos_ipc_ring_claim(ring, buffer, sizeof buffer, &count, &resource_count) == NAOS_IPC_STATUS_OK);
    assert(count == 8 && std::memcmp(buffer, long_message, 8) == 0);
    assert(naos_ipc_ring_commit(ring) == NAOS_IPC_STATUS_OK);

    const auto notifications_before = notifications.load();
    naos_ipc_ring_close(ring, 0);
    assert(notifications.load() > notifications_before);
    assert((naos_ipc_ring_signals(ring, 1) & NAOS_IPC_SIGNAL_PEER_CLOSED) != 0);
    assert(naos_ipc_ring_claim(ring, buffer, sizeof buffer, &count, &resource_count) == NAOS_IPC_STATUS_PEER_CLOSED);
    naos_ipc_ring_close(ring, 1);
    assert(naos_ipc_ring_send(ring, long_message, sizeof long_message, nullptr, 0) == NAOS_IPC_STATUS_PEER_CLOSED);

    naos_ipc_ring_destroy(ring);
    std::free(storage);
}
