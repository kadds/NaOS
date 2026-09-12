#include <naos/ipc_core.hpp>

#include <atomic>
#include <cassert>
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

void release_token(void *context, void *value) noexcept
{
    if (context != nullptr)
        (*static_cast<std::atomic_uint *>(context))++;
    std::free(value);
}
} // namespace

int main()
{
    test_allocator allocator;
    test_lock domain_lock;
    test_lock channel_lock;
    naos::ipc_core::domain_config domain_config{&allocator, &domain_lock, 128, 4096, 128};
    auto *domain = naos::ipc_core::domain::create(domain_config);
    assert(domain != nullptr);
    naos::ipc_core::channel_config channel_config{domain,  &allocator, &allocator, &channel_lock, nullptr,
                                                  nullptr, nullptr,    2,          128,           4};
    auto *channel = naos::ipc_core::channel::create(channel_config);
    assert(channel != nullptr);
    channel->side_reference_acquired(0);
    channel->side_reference_acquired(1);

    auto *message = naos::ipc_core::message::create(*channel, 4, 1);
    assert(message != nullptr);
    std::memcpy(message->bytes(), "test", 4);
    auto *token = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    assert(token != nullptr);
    *token = 42;
    naos::ipc_core::resource resource(nullptr, token, release_token);
    assert(channel->enqueue(0, *message, &resource, 1) == NA_STATUS_OK);
    assert(!resource.valid());
    naos::ipc_core::message *received = nullptr;
    assert(channel->claim_receive(1, received) == NA_STATUS_OK);
    naos::ipc_core::resource received_resource;
    assert(received->take_resource(0, received_resource) == NA_STATUS_OK);
    assert(*static_cast<std::uint64_t *>(received_resource.value()) == 42);
    received_resource.reset();
    assert(channel->commit_receive(1, *received));
    received->destroy();

    auto *first = naos::ipc_core::message::create(*channel, 1, 0);
    auto *second = naos::ipc_core::message::create(*channel, 1, 0);
    auto *third = naos::ipc_core::message::create(*channel, 1, 0);
    assert(first != nullptr && second != nullptr && third != nullptr);
    assert(channel->enqueue(0, *first, nullptr, 0) == NA_STATUS_OK);
    assert(channel->enqueue(0, *second, nullptr, 0) == NA_STATUS_OK);
    assert(channel->enqueue(0, *third, nullptr, 0) == NA_STATUS_WOULD_BLOCK);
    third->destroy();
    assert(channel->claim_receive(1, received) == NA_STATUS_OK);
    assert(channel->cancel_receive(1, *received));
    assert(channel->claim_receive(1, received) == NA_STATUS_OK);
    assert(channel->commit_receive(1, *received));
    received->destroy();
    assert(channel->claim_receive(1, received) == NA_STATUS_OK);
    assert(channel->commit_receive(1, *received));
    received->destroy();

    std::atomic_uint released{0};
    auto *discarded_message = naos::ipc_core::message::create(*channel, 0, 1);
    auto *discarded_token = std::malloc(sizeof(std::uint64_t));
    assert(discarded_message != nullptr && discarded_token != nullptr);
    naos::ipc_core::resource discarded_resource(&released, discarded_token, release_token);
    assert(channel->enqueue(0, *discarded_message, &discarded_resource, 1) == NA_STATUS_OK);
    assert(channel->discard(1, received));
    received->destroy();
    assert(released == 1);

    constexpr unsigned concurrent_messages = 32;
    std::atomic_uint received_count{0};
    std::thread producer([&] {
        for (unsigned i = 0; i < concurrent_messages; i++)
        {
            auto *value = naos::ipc_core::message::create(*channel, 0, 0);
            assert(value != nullptr);
            while (channel->enqueue(0, *value, nullptr, 0) == NA_STATUS_WOULD_BLOCK)
                std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        while (received_count.load() != concurrent_messages)
        {
            naos::ipc_core::message *value = nullptr;
            if (channel->claim_receive(1, value) != NA_STATUS_OK)
            {
                std::this_thread::yield();
                continue;
            }
            assert(channel->commit_receive(1, *value));
            value->destroy();
            received_count++;
        }
    });
    producer.join();
    consumer.join();

    channel->side_reference_released(0);
    assert((channel->signals(1) & NA_SIGNAL_PEER_CLOSED) != 0);
    channel->side_reference_released(1);
    channel->destroy();
    domain->destroy();
}
