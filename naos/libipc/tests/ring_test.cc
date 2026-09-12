#include <naos/ipc_core.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
class test_allocator final : public naos::ipc_core::allocator
{
  public:
    void *allocate(std::size_t size, std::size_t alignment) noexcept override
    {
        if (alignment < sizeof(void *))
            alignment = sizeof(void *);
        void *pointer = nullptr;
        return ::posix_memalign(&pointer, alignment, size) == 0 ? pointer : nullptr;
    }

    void deallocate(void *pointer, std::size_t, std::size_t) noexcept override { std::free(pointer); }
};

class test_lock final : public naos::ipc_core::lock
{
  public:
    void acquire() noexcept override
    {
        while (held_.test_and_set(std::memory_order_acquire))
            std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    void release() noexcept override { held_.clear(std::memory_order_release); }

  private:
    std::atomic_flag held_ = ATOMIC_FLAG_INIT;
};

class test_notifier final : public naos::ipc_core::wait_notifier
{
  public:
    explicit test_notifier(std::atomic_uint &count) noexcept
        : count_(count)
    {
    }

    void notify() noexcept override { count_.fetch_add(1, std::memory_order_relaxed); }

  private:
    std::atomic_uint &count_;
};

void release_token(void *context, void *value) noexcept
{
    if (context != nullptr)
        static_cast<std::atomic_uint *>(context)->fetch_add(1, std::memory_order_relaxed);
    std::free(value);
}

void release_record(naos::ipc_core::resource_record &record) noexcept
{
    if (record.valid() && record.release != nullptr)
        record.release(record.context, record.value);
    record.clear();
}

using ring = naos::ipc_core::ring;

struct fixture
{
    test_allocator allocator;
    test_lock lock;
    std::atomic_uint notifications{0};
    test_notifier notifier{notifications};
    void *storage = nullptr;
    ring *instance = nullptr;

    fixture(std::uint64_t slot_bytes, std::uint32_t capacity, std::uint32_t resources)
    {
        const std::uint64_t bytes = ring::required_bytes(slot_bytes, capacity, resources);
        assert(bytes != 0);
        void *pointer = nullptr;
        assert(::posix_memalign(&pointer, 64, bytes) == 0);
        storage = pointer;
        std::memset(storage, 0, bytes);
        assert(ring::format(storage, bytes, slot_bytes, capacity, resources) == NA_STATUS_OK);
        naos::ipc_core::ring_config config;
        config.control_memory = &allocator;
        config.synchronization = &lock;
        config.notifier = &notifier;
        config.storage = storage;
        config.storage_bytes = bytes;
        config.slot_bytes = slot_bytes;
        config.slot_capacity = capacity;
        config.resource_capacity = resources;
        instance = ring::create(config);
        assert(instance != nullptr);
    }

    ~fixture()
    {
        if (instance != nullptr)
            instance->destroy();
        std::free(storage);
    }
};

void test_empty_and_full_status_codes()
{
    fixture f(16, 2, 0);
    std::uint8_t buffer[16] = {};
    std::uint64_t count = 99;
    std::uint64_t resources = 99;

    // Empty: the consumer is told to retry rather than handed stale bytes.
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_WOULD_BLOCK);
    assert(count == 0 && resources == 0);
    assert(f.instance->queued() == 0);
    assert((f.instance->signals(0) & NA_SIGNAL_WRITABLE) != 0);
    assert((f.instance->signals(1) & NA_SIGNAL_READABLE) == 0);

    const std::uint8_t bytes[3] = {1, 2, 3};
    assert(f.instance->send(bytes, 3) == NA_STATUS_OK);
    assert(f.instance->send(bytes, 3) == NA_STATUS_OK);
    assert(f.instance->queued() == 2);
    assert((f.instance->signals(0) & NA_SIGNAL_WRITABLE) == 0);
    assert((f.instance->signals(1) & NA_SIGNAL_READABLE) != 0);

    // Full: the producer is told to retry, not corrupted or dropped.
    assert(f.instance->send(bytes, 3) == NA_STATUS_WOULD_BLOCK);
    assert(f.instance->queued() == 2);

    // Oversize payload and resource counts are rejected before touching state.
    fixture small(8, 1, 1);
    std::uint8_t oversize[9] = {};
    assert(small.instance->send(oversize, sizeof oversize) == NA_STATUS_INVALID_MESSAGE);
    naos::ipc_core::resource_record pair[2] = {{&f.notifications, std::malloc(1), release_token}, {}};
    assert(small.instance->send(bytes, 1, pair, 2) == NA_STATUS_INVALID_MESSAGE);
    release_record(pair[0]);
}

void test_claim_commit_and_cancel()
{
    fixture f(16, 2, 0);
    const std::uint8_t bytes[3] = {1, 2, 3};
    std::uint8_t buffer[16] = {};
    std::uint64_t count = 0;
    std::uint64_t resources = 0;

    assert(f.instance->commit() == NA_STATUS_INVALID_HANDLE);
    assert(f.instance->cancel() == NA_STATUS_INVALID_HANDLE);
    assert(!f.instance->claim_pending());

    assert(f.instance->send(bytes, 3) == NA_STATUS_OK);
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_OK);
    assert(f.instance->claim_pending());
    // A second claim while one is pinned is refused, not served out of order.
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_WOULD_BLOCK);
    assert(f.instance->commit() == NA_STATUS_OK);
    assert(!f.instance->claim_pending());
    assert(f.instance->queued() == 0);

    // Cancel discards the pinned entry so the ring cannot wedge.
    assert(f.instance->send(bytes, 3) == NA_STATUS_OK);
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_OK);
    assert(f.instance->cancel() == NA_STATUS_OK);
    assert(f.instance->queued() == 0);
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_WOULD_BLOCK);
}

void test_buffer_too_small_reports_required()
{
    fixture f(32, 2, 0);
    const std::uint8_t message[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    assert(f.instance->send(message, sizeof message) == NA_STATUS_OK);

    std::uint8_t buffer[32] = {};
    std::uint64_t count = 0;
    std::uint64_t resources = 0;
    assert(f.instance->claim(buffer, 4, count, resources) == NA_STATUS_BUFFER_TOO_SMALL);
    assert(count == 8);
    // The failed claim did not consume: a retry with room sees the same entry.
    assert(!f.instance->claim_pending());
    assert(f.instance->queued() == 1);
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_OK);
    assert(count == 8 && std::memcmp(buffer, message, 8) == 0);
    assert(f.instance->commit() == NA_STATUS_OK);
}

void test_sequential_ordering()
{
    fixture f(8, 4, 0);
    for (std::uint8_t value = 0; value < 4; value++)
        assert(f.instance->send(&value, 1) == NA_STATUS_OK);
    for (std::uint8_t expected = 0; expected < 4; expected++)
    {
        std::uint8_t buffer[8] = {};
        std::uint64_t count = 0;
        std::uint64_t resources = 0;
        assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_OK);
        assert(count == 1 && buffer[0] == expected);
        assert(f.instance->commit() == NA_STATUS_OK);
    }
}

void test_concurrent_producer_consumer()
{
    constexpr unsigned total = 20000;
    fixture f(16, 8, 0);
    std::atomic<unsigned> received{0};
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        for (unsigned sequence = 0; sequence < total; sequence++)
        {
            const std::uint32_t value = sequence * 2654435761u;
            const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
            while (f.instance->send(bytes, sizeof value) == NA_STATUS_WOULD_BLOCK)
                std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        unsigned expected = 0;
        while (expected < total)
        {
            std::uint8_t buffer[16] = {};
            std::uint64_t count = 0;
            std::uint64_t resources = 0;
            const auto status = f.instance->claim(buffer, sizeof buffer, count, resources);
            if (status == NA_STATUS_WOULD_BLOCK)
            {
                std::this_thread::yield();
                continue;
            }
            std::uint32_t value = 0;
            std::memcpy(&value, buffer, sizeof value);
            if (status != NA_STATUS_OK || count != sizeof value || value != expected * 2654435761u)
            {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            if (f.instance->commit() != NA_STATUS_OK)
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
    assert(f.instance->queued() == 0);
}

void test_acquire_release_pattern()
{
    // Every byte of a full-slot message is a function of its sequence number,
    // so a torn payload, a stale slot, or a slot reused before its release
    // becomes visible all show up as a mismatch on the consumer side.
    constexpr unsigned total = 5000;
    constexpr std::uint64_t slot_bytes = 64;
    fixture f(slot_bytes, 4, 0);
    std::atomic<bool> failed{false};
    std::atomic<unsigned> received{0};

    std::thread producer([&] {
        std::uint8_t buffer[slot_bytes] = {};
        for (unsigned sequence = 0; sequence < total; sequence++)
        {
            for (std::uint64_t index = 0; index < slot_bytes; index++)
                buffer[index] = static_cast<std::uint8_t>((sequence * 131u + index * 17u + 1u) & 0xFFu);
            while (f.instance->send(buffer, slot_bytes) == NA_STATUS_WOULD_BLOCK)
                std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        unsigned expected = 0;
        while (expected < total)
        {
            std::uint8_t buffer[slot_bytes] = {};
            std::uint64_t count = 0;
            std::uint64_t resources = 0;
            const auto status = f.instance->claim(buffer, sizeof buffer, count, resources);
            if (status == NA_STATUS_WOULD_BLOCK)
            {
                std::this_thread::yield();
                continue;
            }
            if (status != NA_STATUS_OK || count != slot_bytes)
            {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            for (std::uint64_t index = 0; index < slot_bytes; index++)
            {
                const auto wanted = static_cast<std::uint8_t>((expected * 131u + index * 17u + 1u) & 0xFFu);
                if (buffer[index] != wanted)
                {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
            if (f.instance->commit() != NA_STATUS_OK)
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
}

void test_consumer_copies_before_validate()
{
    // A single-slot ring: while a claim is pinned, the producer cannot touch
    // the slot at all.  The consumer validates its private copy, so even a
    // producer that rewrites its source buffer afterwards cannot make the
    // consumer act on data it did not validate.
    fixture f(16, 1, 0);
    std::uint8_t source[16] = {};
    for (std::uint64_t index = 0; index < sizeof source; index++)
        source[index] = static_cast<std::uint8_t>(index * 3 + 1);
    assert(f.instance->send(source, sizeof source) == NA_STATUS_OK);

    std::uint8_t copy[16] = {};
    std::uint64_t count = 0;
    std::uint64_t resources = 0;
    assert(f.instance->claim(copy, sizeof copy, count, resources) == NA_STATUS_OK);
    assert(count == sizeof source);
    assert(f.instance->claim_pending());

    // The pinned slot is not recyclable, so the producer is backpressured.
    assert(f.instance->send(source, sizeof source) == NA_STATUS_WOULD_BLOCK);

    // Model a producer that overwrites the source after publishing: the
    // consumer validates the bytes it already copied, not the live slot.
    std::memset(source, 0xFF, sizeof source);
    std::uint8_t checksum = 0;
    for (std::uint64_t index = 0; index < sizeof copy; index++)
        checksum += copy[index];
    std::uint8_t expected = 0;
    for (std::uint64_t index = 0; index < sizeof copy; index++)
        expected = static_cast<std::uint8_t>(expected + index * 3 + 1);
    assert(checksum == expected);

    assert(f.instance->commit() == NA_STATUS_OK);
    // Only now may the producer reuse the slot.
    std::memset(source, 0xAB, sizeof source);
    assert(f.instance->send(source, sizeof source) == NA_STATUS_OK);
    assert(f.instance->claim(copy, sizeof copy, count, resources) == NA_STATUS_OK);
    assert(count == sizeof copy && copy[0] == 0xAB && copy[15] == 0xAB);
    assert(f.instance->commit() == NA_STATUS_OK);
}

void test_resource_transfer()
{
    fixture f(16, 2, 2);
    std::atomic_uint released{0};
    std::uint64_t first_value = 11;
    std::uint64_t second_value = 22;
    auto *first = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    auto *second = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    assert(first != nullptr && second != nullptr);
    *first = first_value;
    *second = second_value;
    naos::ipc_core::resource_record records[2] = {
        {&released, first, release_token},
        {&released, second, release_token},
    };
    const std::uint8_t payload[2] = {7, 8};
    assert(f.instance->send(payload, sizeof payload, records, 2) == NA_STATUS_OK);
    // Ownership moved out of the caller's records.
    assert(!records[0].valid() && !records[1].valid());

    std::uint8_t buffer[16] = {};
    std::uint64_t count = 0;
    std::uint64_t resources = 0;
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_OK);
    assert(count == 2 && resources == 2);

    naos::ipc_core::resource_record taken;
    assert(f.instance->take_resource(1, taken) == NA_STATUS_OK);
    assert(*static_cast<std::uint64_t *>(taken.value) == second_value);
    release_record(taken);
    assert(released.load() == 1);
    // Taking the same record twice is refused.
    assert(f.instance->take_resource(1, taken) == NA_STATUS_INVALID_HANDLE);
    assert(f.instance->take_resource(9, taken) == NA_STATUS_INVALID_ARGUMENT);

    // Commit releases the resource the consumer chose not to take.
    assert(f.instance->commit() == NA_STATUS_OK);
    assert(released.load() == 2);

    // An untaken resource on a cancelled entry is released too.
    auto *third = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    auto *fourth = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    assert(third != nullptr && fourth != nullptr);
    *third = 33;
    *fourth = 44;
    naos::ipc_core::resource_record next[2] = {
        {&released, third, release_token},
        {&released, fourth, release_token},
    };
    assert(f.instance->send(payload, sizeof payload, next, 2) == NA_STATUS_OK);
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_OK);
    assert(f.instance->take_resource(0, taken) == NA_STATUS_OK);
    release_record(taken);
    assert(f.instance->cancel() == NA_STATUS_OK);
    assert(released.load() == 4);

    // Invalid records and unclaimed takes are rejected.
    naos::ipc_core::resource_record invalid{};
    assert(f.instance->send(payload, sizeof payload, &invalid, 1) == NA_STATUS_INVALID_ARGUMENT);
    assert(f.instance->take_resource(0, taken) == NA_STATUS_INVALID_HANDLE);
    assert(f.instance->commit() == NA_STATUS_INVALID_HANDLE);
}

void test_close_unblocks_peer()
{
    fixture f(16, 2, 0);
    const std::uint8_t payload[3] = {1, 2, 3};
    assert(f.instance->send(payload, 3) == NA_STATUS_OK);

    assert((f.instance->signals(1) & NA_SIGNAL_PEER_CLOSED) == 0);
    const auto before = f.notifications.load();
    f.instance->close(0);
    assert(f.notifications.load() > before);
    assert((f.instance->signals(1) & NA_SIGNAL_PEER_CLOSED) != 0);
    assert((f.instance->signals(0) & NA_SIGNAL_PEER_CLOSED) == 0);

    // Published entries still drain; only the empty claim reports the close.
    std::uint8_t buffer[16] = {};
    std::uint64_t count = 0;
    std::uint64_t resources = 0;
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_OK);
    assert(count == 3);
    assert(f.instance->commit() == NA_STATUS_OK);
    assert(f.instance->claim(buffer, sizeof buffer, count, resources) == NA_STATUS_PEER_CLOSED);

    fixture g(16, 2, 0);
    g.instance->close(1);
    assert((g.instance->signals(0) & NA_SIGNAL_PEER_CLOSED) != 0);
    assert(g.instance->send(payload, 3) == NA_STATUS_PEER_CLOSED);
}

void test_format_and_geometry()
{
    const std::uint64_t small_bytes = ring::required_bytes(16, 2, 1);
    const std::uint64_t large_bytes = ring::required_bytes(32, 2, 1);
    assert(small_bytes != 0 && large_bytes > small_bytes);
    void *storage = nullptr;
    assert(::posix_memalign(&storage, 64, large_bytes) == 0);
    std::memset(storage, 0, large_bytes);

    // A buffer one byte short cannot be formatted.
    assert(ring::format(storage, small_bytes - 1, 16, 2, 1) == NA_STATUS_BUFFER_TOO_SMALL);
    // Unformatted storage cannot be attached.
    test_allocator allocator;
    test_lock lock;
    std::atomic_uint notifications{0};
    test_notifier notifier(notifications);
    naos::ipc_core::ring_config config;
    config.control_memory = &allocator;
    config.synchronization = &lock;
    config.notifier = &notifier;
    config.storage = storage;
    config.storage_bytes = small_bytes;
    config.slot_bytes = 16;
    config.slot_capacity = 2;
    config.resource_capacity = 1;
    assert(ring::create(config) == nullptr);

    assert(ring::format(storage, small_bytes, 16, 2, 1) == NA_STATUS_OK);
    // Formatting twice with the same geometry is a no-op, a different
    // geometry on live storage is refused.
    assert(ring::format(storage, small_bytes, 16, 2, 1) == NA_STATUS_OK);
    assert(ring::format(storage, large_bytes, 32, 2, 1) == NA_STATUS_INVALID_ARGUMENT);

    auto *instance = ring::create(config);
    assert(instance != nullptr);
    assert(instance->slot_capacity() == 2);
    assert(instance->slot_bytes() == 16);
    assert(instance->resource_capacity() == 1);
    instance->destroy();

    // Geometry mismatches and impossible parameters are rejected.
    naos::ipc_core::ring_config mismatched = config;
    mismatched.slot_bytes = 32;
    mismatched.storage_bytes = large_bytes;
    assert(ring::create(mismatched) == nullptr);
    assert(ring::required_bytes(0, 2, 1) == 0);
    assert(ring::required_bytes(16, 0, 1) == 0);
    assert(ring::required_bytes(16, 100000, 1) == 0);
    assert(ring::required_bytes(16, 2, 100000) == 0);
    assert(ring::format(nullptr, small_bytes, 16, 2, 1) == NA_STATUS_INVALID_ARGUMENT);
    std::free(storage);
}
} // namespace

int main()
{
    test_empty_and_full_status_codes();
    test_claim_commit_and_cancel();
    test_buffer_too_small_reports_required();
    test_sequential_ordering();
    test_concurrent_producer_consumer();
    test_acquire_release_pattern();
    test_consumer_copies_before_validate();
    test_resource_transfer();
    test_close_unblocks_peer();
    test_format_and_geometry();
}
