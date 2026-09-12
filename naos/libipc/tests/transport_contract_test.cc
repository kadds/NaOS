#include <naos/ipc_core.h>
#include <naos/ipc_core.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

/*
 * One contract body, two transports.
 *
 * The ring is an optional data-plane transport next to the accepted channel
 * state machine, so both must agree on the observable contract for the
 * behaviours they share: FIFO delivery, empty/full backpressure, resource
 * transfer on commit, required-size reporting, and peer close.  Running the
 * same body against each keeps the ring from quietly diverging.
 */
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

class transport
{
  public:
    virtual ~transport() = default;

    virtual std::uint64_t capacity() const = 0;
    virtual std::uint64_t queued() const = 0;
    virtual std::uint64_t released() const = 0;
    virtual naos_ipc_status_t enqueue(const std::uint8_t *bytes, std::uint64_t count) = 0;
    virtual naos_ipc_status_t enqueue_with_resource(const std::uint8_t *bytes, std::uint64_t count, void *value) = 0;
    virtual naos_ipc_status_t receive(std::uint8_t *out, std::uint64_t capacity, std::uint64_t &count,
                                      std::uint64_t &resources) = 0;
    virtual naos_ipc_status_t take_resource(std::uint64_t index, naos_ipc_resource_t &out) = 0;
    virtual naos_ipc_status_t commit() = 0;
    virtual std::uint64_t producer_signals() const = 0;
    virtual std::uint64_t consumer_signals() const = 0;
    virtual void close_producer() = 0;
    virtual void close_consumer() = 0;

  protected:
    std::atomic_uint released_{0};
};

class channel_transport final : public transport
{
  public:
    channel_transport()
    {
        naos::ipc_core::domain_config domain_config{&allocator_, &domain_lock_, 16, 4096, 16};
        domain_ = naos::ipc_core::domain::create(domain_config);
        assert(domain_ != nullptr);
        naos::ipc_core::channel_config channel_config{domain_, &allocator_, &allocator_, &channel_lock_, nullptr,
                                                      nullptr, nullptr,    2,         128,           4};
        channel_ = naos::ipc_core::channel::create(channel_config);
        assert(channel_ != nullptr);
        channel_->side_reference_acquired(0);
        channel_->side_reference_acquired(1);
    }

    ~channel_transport() override
    {
        if (pending_ != nullptr)
        {
            channel_->commit_receive(1, *pending_);
            pending_->destroy();
        }
        if (producer_open_)
            channel_->side_reference_released(0);
        if (consumer_open_)
            channel_->side_reference_released(1);
        channel_->destroy();
        domain_->destroy();
    }

    std::uint64_t capacity() const override { return channel_->max_messages(); }
    std::uint64_t queued() const override { return channel_->queued_messages(1); }
    std::uint64_t released() const override { return released_.load(); }

    naos_ipc_status_t enqueue(const std::uint8_t *bytes, std::uint64_t count) override
    {
        auto *message = naos::ipc_core::message::create(*channel_, count, 0);
        assert(message != nullptr);
        if (count != 0)
            std::memcpy(message->bytes(), bytes, count);
        const auto status = channel_->enqueue(0, *message, nullptr, 0);
        if (status != NA_STATUS_OK)
            message->destroy();
        return static_cast<naos_ipc_status_t>(status);
    }

    naos_ipc_status_t enqueue_with_resource(const std::uint8_t *bytes, std::uint64_t count, void *value) override
    {
        auto *message = naos::ipc_core::message::create(*channel_, count, 1);
        assert(message != nullptr);
        if (count != 0)
            std::memcpy(message->bytes(), bytes, count);
        naos::ipc_core::resource resource(&released_, value, release_token);
        const auto status = channel_->enqueue(0, *message, &resource, 1);
        if (status != NA_STATUS_OK)
        {
            resource.reset();
            message->destroy();
        }
        return static_cast<naos_ipc_status_t>(status);
    }

    naos_ipc_status_t receive(std::uint8_t *out, std::uint64_t capacity, std::uint64_t &count,
                              std::uint64_t &resources) override
    {
        naos::ipc_core::message *message = nullptr;
        const auto status = channel_->claim_receive(1, message);
        if (status != NA_STATUS_OK)
            return static_cast<naos_ipc_status_t>(status);
        count = message->byte_count();
        resources = message->resource_count();
        if (count > capacity)
        {
            channel_->cancel_receive(1, *message);
            return NAOS_IPC_STATUS_BUFFER_TOO_SMALL;
        }
        if (count != 0)
            std::memcpy(out, message->bytes(), count);
        pending_ = message;
        return NAOS_IPC_STATUS_OK;
    }

    naos_ipc_status_t take_resource(std::uint64_t index, naos_ipc_resource_t &out) override
    {
        assert(pending_ != nullptr);
        naos::ipc_core::resource value;
        const auto status = pending_->take_resource(index, value);
        if (status != NA_STATUS_OK)
            return static_cast<naos_ipc_status_t>(status);
        out.context = value.context();
        out.value = value.value();
        out.release = value.release_function();
        value.take_value();
        return NAOS_IPC_STATUS_OK;
    }

    naos_ipc_status_t commit() override
    {
        assert(pending_ != nullptr);
        const auto status = channel_->commit_receive(1, *pending_) ? NAOS_IPC_STATUS_OK : NAOS_IPC_STATUS_INVALID_HANDLE;
        pending_->destroy();
        pending_ = nullptr;
        return status;
    }

    std::uint64_t producer_signals() const override { return channel_->signals(0); }
    std::uint64_t consumer_signals() const override { return channel_->signals(1); }

    void close_producer() override
    {
        if (producer_open_)
        {
            channel_->side_reference_released(0);
            producer_open_ = false;
        }
    }

    void close_consumer() override
    {
        if (consumer_open_)
        {
            channel_->side_reference_released(1);
            consumer_open_ = false;
        }
    }

  private:
    test_allocator allocator_;
    test_lock domain_lock_;
    test_lock channel_lock_;
    naos::ipc_core::domain *domain_ = nullptr;
    naos::ipc_core::channel *channel_ = nullptr;
    naos::ipc_core::message *pending_ = nullptr;
    bool producer_open_ = true;
    bool consumer_open_ = true;
};

class ring_transport final : public transport
{
  public:
    ring_transport()
    {
        const std::uint64_t bytes = naos::ipc_core::ring::required_bytes(slot_bytes_, capacity_, resource_capacity_);
        assert(bytes != 0);
        void *pointer = nullptr;
        assert(::posix_memalign(&pointer, 64, bytes) == 0);
        storage_ = pointer;
        std::memset(storage_, 0, bytes);
        assert(naos::ipc_core::ring::format(storage_, bytes, slot_bytes_, capacity_, resource_capacity_) ==
               NA_STATUS_OK);
        naos::ipc_core::ring_config config;
        config.control_memory = &allocator_;
        config.synchronization = &lock_;
        config.notifier = &notifier_;
        config.storage = storage_;
        config.storage_bytes = bytes;
        config.slot_bytes = slot_bytes_;
        config.slot_capacity = capacity_;
        config.resource_capacity = resource_capacity_;
        ring_ = naos::ipc_core::ring::create(config);
        assert(ring_ != nullptr);
    }

    ~ring_transport() override
    {
        ring_->destroy();
        std::free(storage_);
    }

    std::uint64_t capacity() const override { return ring_->slot_capacity(); }
    std::uint64_t queued() const override { return ring_->queued(); }
    std::uint64_t released() const override { return released_.load(); }

    naos_ipc_status_t enqueue(const std::uint8_t *bytes, std::uint64_t count) override
    {
        return static_cast<naos_ipc_status_t>(ring_->send(bytes, count));
    }

    naos_ipc_status_t enqueue_with_resource(const std::uint8_t *bytes, std::uint64_t count, void *value) override
    {
        naos::ipc_core::resource_record record{&released_, value, release_token};
        return static_cast<naos_ipc_status_t>(ring_->send(bytes, count, &record, 1));
    }

    naos_ipc_status_t receive(std::uint8_t *out, std::uint64_t capacity, std::uint64_t &count,
                              std::uint64_t &resources) override
    {
        return static_cast<naos_ipc_status_t>(ring_->claim(out, capacity, count, resources));
    }

    naos_ipc_status_t take_resource(std::uint64_t index, naos_ipc_resource_t &out) override
    {
        naos::ipc_core::resource_record record;
        const auto status = ring_->take_resource(index, record);
        if (status != NA_STATUS_OK)
            return static_cast<naos_ipc_status_t>(status);
        out.context = record.context;
        out.value = record.value;
        out.release = record.release;
        record.clear();
        return NAOS_IPC_STATUS_OK;
    }

    naos_ipc_status_t commit() override { return static_cast<naos_ipc_status_t>(ring_->commit()); }
    std::uint64_t producer_signals() const override { return ring_->signals(0); }
    std::uint64_t consumer_signals() const override { return ring_->signals(1); }
    void close_producer() override { ring_->close(0); }
    void close_consumer() override { ring_->close(1); }

  private:
    static constexpr std::uint64_t slot_bytes_ = 64;
    static constexpr std::uint32_t capacity_ = 2;
    static constexpr std::uint32_t resource_capacity_ = 1;

    test_allocator allocator_;
    test_lock lock_;
    std::atomic_uint notifications_{0};
    test_notifier notifier_{notifications_};
    void *storage_ = nullptr;
    naos::ipc_core::ring *ring_ = nullptr;
};

void run_contract(transport &t)
{
    std::uint8_t buffer[64] = {};
    std::uint64_t count = 0;
    std::uint64_t resources = 0;

    // Empty transport reports retry, not a stale entry.
    assert(t.receive(buffer, sizeof buffer, count, resources) == NAOS_IPC_STATUS_WOULD_BLOCK);
    assert(count == 0 && resources == 0);
    assert(t.queued() == 0);
    assert((t.producer_signals() & NAOS_IPC_SIGNAL_WRITABLE) != 0);
    assert((t.consumer_signals() & NAOS_IPC_SIGNAL_READABLE) == 0);

    // FIFO and byte preservation.
    const std::uint8_t first[3] = {4, 5, 6};
    const std::uint8_t second[2] = {7, 8};
    assert(t.enqueue(first, sizeof first) == NAOS_IPC_STATUS_OK);
    assert(t.enqueue(second, sizeof second) == NAOS_IPC_STATUS_OK);
    assert(t.receive(buffer, sizeof buffer, count, resources) == NAOS_IPC_STATUS_OK);
    assert(count == sizeof first && resources == 0 && std::memcmp(buffer, first, sizeof first) == 0);
    assert(t.commit() == NAOS_IPC_STATUS_OK);
    assert(t.receive(buffer, sizeof buffer, count, resources) == NAOS_IPC_STATUS_OK);
    assert(count == sizeof second && std::memcmp(buffer, second, sizeof second) == 0);
    assert(t.commit() == NAOS_IPC_STATUS_OK);

    // Resource transfer survives the transport and is released exactly once.
    auto *token = static_cast<std::uint64_t *>(std::malloc(sizeof(std::uint64_t)));
    assert(token != nullptr);
    *token = 0x5A5A;
    assert(t.enqueue_with_resource(first, sizeof first, token) == NAOS_IPC_STATUS_OK);
    assert(t.receive(buffer, sizeof buffer, count, resources) == NAOS_IPC_STATUS_OK);
    assert(count == sizeof first && resources == 1);
    naos_ipc_resource_t taken{};
    assert(t.take_resource(0, taken) == NAOS_IPC_STATUS_OK);
    assert(taken.value == token && *static_cast<std::uint64_t *>(taken.value) == 0x5A5A);
    naos_ipc_resource_reset(&taken);
    assert(t.released() == 1);
    assert(t.commit() == NAOS_IPC_STATUS_OK);

    // Backpressure at the capacity bound, and recovery after a receive.
    while (t.enqueue(first, 1) == NAOS_IPC_STATUS_OK)
    {
    }
    assert(t.queued() == t.capacity());
    assert(t.enqueue(first, 1) == NAOS_IPC_STATUS_WOULD_BLOCK);
    assert((t.producer_signals() & NAOS_IPC_SIGNAL_WRITABLE) == 0);
    assert(t.receive(buffer, sizeof buffer, count, resources) == NAOS_IPC_STATUS_OK);
    assert(t.commit() == NAOS_IPC_STATUS_OK);
    assert(t.enqueue(first, 1) == NAOS_IPC_STATUS_OK);
    while (t.queued() != 0)
    {
        assert(t.receive(buffer, sizeof buffer, count, resources) == NAOS_IPC_STATUS_OK);
        assert(t.commit() == NAOS_IPC_STATUS_OK);
    }

    // A too-small destination reports the required size and consumes nothing.
    const std::uint8_t long_message[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    assert(t.enqueue(long_message, sizeof long_message) == NAOS_IPC_STATUS_OK);
    assert(t.receive(buffer, 4, count, resources) == NAOS_IPC_STATUS_BUFFER_TOO_SMALL);
    assert(count == sizeof long_message);
    assert(t.receive(buffer, sizeof buffer, count, resources) == NAOS_IPC_STATUS_OK);
    assert(count == sizeof long_message && std::memcmp(buffer, long_message, sizeof long_message) == 0);
    assert(t.commit() == NAOS_IPC_STATUS_OK);

    // Closing one side is visible to the other and retires empty receives.
    t.close_producer();
    assert((t.consumer_signals() & NAOS_IPC_SIGNAL_PEER_CLOSED) != 0);
    assert(t.receive(buffer, sizeof buffer, count, resources) == NAOS_IPC_STATUS_PEER_CLOSED);
    t.close_consumer();
    assert((t.producer_signals() & NAOS_IPC_SIGNAL_PEER_CLOSED) != 0);
}
} // namespace

int main()
{
    channel_transport channel;
    run_contract(channel);

    ring_transport ring;
    run_contract(ring);
}
