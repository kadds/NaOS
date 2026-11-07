#include "kernel/arch/klib.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>
#include <limits>

namespace naos::ipc
{
namespace
{
constexpr u64 no_deadline = 0;

struct deadline_watch
{
    handle_t<invocation_state> state;

    explicit deadline_watch(const handle_t<invocation_state> &state)
        : state(state)
    {
    }

    void invoke(timeclock::microsecond_t) noexcept
    {
        if (state)
        {
            state->expire_deadline();
            state->deadline_fired();
        }
        state.reset();
        memory::Delete<>(memory::KernelCommonAllocatorV, this);
    }
};

u64 calculate_deadline(u64 budget)
{
    if (budget == 0)
        return no_deadline;
    const u64 now = timer::get_high_resolution_time();
    if (budget > std::numeric_limits<u64>::max() - now)
        return std::numeric_limits<u64>::max();
    return now + budget;
}

} // namespace

namespace
{
std::atomic<naos_ipc_domain_t *> invocation_core_domain{nullptr};
core_lock invocation_core_domain_lock;

void *invocation_control_allocate(void *, std::size_t size, std::size_t alignment)
{
    return memory::KernelCommonAllocatorV->allocate(size, alignment);
}

void invocation_control_deallocate(void *, void *pointer, std::size_t, std::size_t)
{
    if (pointer != nullptr)
        memory::KernelCommonAllocatorV->deallocate(pointer);
}

void *invocation_payload_allocate(void *, std::size_t size, std::size_t alignment)
{
    return memory::MemoryAllocatorV->allocate(size, alignment);
}

void invocation_payload_deallocate(void *, void *pointer, std::size_t, std::size_t)
{
    if (pointer != nullptr)
        memory::MemoryAllocatorV->deallocate(pointer);
}

std::uint64_t invocation_clock_now(void *) { return timer::get_high_resolution_time(); }

naos_ipc_allocator_t invocation_control_allocator{nullptr, invocation_control_allocate, invocation_control_deallocate};
naos_ipc_allocator_t invocation_payload_allocator{nullptr, invocation_payload_allocate, invocation_payload_deallocate};
naos_ipc_lock_t invocation_domain_lock_api{&invocation_core_domain_lock, core_lock_acquire, core_lock_release};

naos_ipc_domain_t *get_invocation_core_domain()
{
    auto *domain = invocation_core_domain.load(std::memory_order_acquire);
    if (domain != nullptr)
        return domain;

    naos_ipc_domain_config_t config{};
    config.memory = &invocation_control_allocator;
    config.synchronization = &invocation_domain_lock_api;
    config.max_messages = NA_CHANNEL_GLOBAL_MAX_MESSAGES;
    config.max_bytes = NA_CHANNEL_GLOBAL_MAX_BYTES;
    config.max_resources = NA_CHANNEL_GLOBAL_MAX_RESOURCES;
    auto *candidate = naos_ipc_domain_create(&config);
    if (candidate == nullptr)
        return nullptr;
    domain = nullptr;
    if (invocation_core_domain.compare_exchange_strong(domain, candidate, std::memory_order_release,
                                                       std::memory_order_acquire))
        return candidate;
    naos_ipc_domain_destroy(candidate);
    return domain;
}

void kernel_invocation_notify(void *context) noexcept
{
    static_cast<invocation_state *>(context)->notify_core_waiters();
}

void kernel_invocation_wake_execution(void *context) noexcept
{
    static_cast<invocation_state *>(context)->wake_core_execution();
}

int kernel_invocation_remove_queued(void *context) noexcept
{
    return static_cast<invocation_state *>(context)->remove_queued_from_core() ? 1 : 0;
}

struct invocation_resource_holder
{
    capability::transferred_resource resource;
};

void release_invocation_resource(void *, void *value) noexcept
{
    auto *holder = static_cast<invocation_resource_holder *>(value);
    if (holder != nullptr)
        memory::Delete<>(memory::KernelCommonAllocatorV, holder);
}

void clear_invocation_resource(naos_ipc_resource_t &resource)
{
    resource.context = nullptr;
    resource.value = nullptr;
    resource.release = nullptr;
}

bool make_invocation_resource_batch(capability::transfer_record_list &records,
                                    freelibcxx::vector<naos_ipc_resource_t> &resources)
{
    resources.ensure(records.size());
    if (records.size() != 0 && resources.data() == nullptr)
        return false;
    for (u64 index = 0; index < records.size(); index++)
    {
        if (!records[index].resource.valid())
        {
            resources.push_back(naos_ipc_resource_t{});
            continue;
        }
        auto *holder = memory::New<invocation_resource_holder>(memory::KernelCommonAllocatorV);
        if (holder == nullptr)
        {
            for (u64 previous_index = 0; previous_index < resources.size(); previous_index++)
            {
                auto *previous = static_cast<invocation_resource_holder *>(resources[previous_index].value);
                if (previous == nullptr)
                    continue;
                clear_invocation_resource(resources[previous_index]);
                records[previous_index].resource = std::move(previous->resource);
                memory::Delete<>(memory::KernelCommonAllocatorV, previous);
            }
            return false;
        }
        holder->resource = std::move(records[index].resource);
        resources.push_back(naos_ipc_resource_t{nullptr, holder, release_invocation_resource});
    }
    return true;
}

void restore_invocation_resource_batch(capability::transfer_record_list &records,
                                       freelibcxx::vector<naos_ipc_resource_t> &resources)
{
    for (u64 index = 0; index < resources.size(); index++)
    {
        auto *holder = static_cast<invocation_resource_holder *>(resources[index].value);
        if (holder == nullptr)
            continue;
        clear_invocation_resource(resources[index]);
        records[index].resource = std::move(holder->resource);
        memory::Delete<>(memory::KernelCommonAllocatorV, holder);
    }
}

} // namespace

invocation_state::invocation_state(u64 method_id, u64 operation_budget, u64 max_response_bytes,
                                   u64 max_response_resources)
    : lock_()
    , wait_queue_()
    , execution_wait_queue_(nullptr)
    , queue_owner_(nullptr)
    , core_lock_()
    , core_lock_api_{&core_lock_, core_lock_acquire, core_lock_release}
    , core_notifier_api_{this, kernel_invocation_notify}
    , core_clock_api_{nullptr, invocation_clock_now}
    , core_callbacks_{this, kernel_invocation_remove_queued, kernel_invocation_wake_execution}
    , core_(nullptr)
{
    auto *domain = get_invocation_core_domain();
    if (domain == nullptr)
        return;
    naos_ipc_invocation_config_t config{};
    config.owner_domain = domain;
    config.control_memory = &invocation_control_allocator;
    config.payload_memory = &invocation_payload_allocator;
    config.synchronization = &core_lock_api_;
    config.notifier = &core_notifier_api_;
    config.clock = &core_clock_api_;
    config.callbacks = &core_callbacks_;
    config.method_id = method_id;
    config.operation_deadline = calculate_deadline(operation_budget);
    config.max_response_bytes = max_response_bytes;
    config.max_response_resources = max_response_resources;
    core_ = naos_ipc_invocation_create(&config);
}

invocation_state::~invocation_state()
{
    if (core_ != nullptr)
        naos_ipc_invocation_destroy(core_);
    core_ = nullptr;
}

void invocation_state::deadline_fired()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    deadline_watcher_ = timer::invalid_watcher_id;
    deadline_watch_ = nullptr;
}

void invocation_state::disarm_deadline()
{
    timer::watcher_id watcher = timer::invalid_watcher_id;
    void *watch = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        watcher = deadline_watcher_;
        watch = deadline_watch_;
        deadline_watcher_ = timer::invalid_watcher_id;
        deadline_watch_ = nullptr;
    }

    if (watcher != timer::invalid_watcher_id && timer::cancel(watcher))
        memory::Delete<>(memory::KernelCommonAllocatorV, reinterpret_cast<deadline_watch *>(watch));
}

na_signal_t invocation_state::signals() const { return core_ == nullptr ? 0 : naos_ipc_invocation_signals(core_); }

u64 invocation_state::method_id() const { return core_ == nullptr ? 0 : naos_ipc_invocation_method_id(core_); }

u64 invocation_state::operation_deadline() const
{
    return core_ == nullptr ? 0 : naos_ipc_invocation_operation_deadline(core_);
}

bool invocation_state::begin_receive() { return core_ != nullptr && naos_ipc_invocation_begin_receive(core_) != 0; }

void invocation_state::rollback_receive()
{
    if (core_ != nullptr)
        naos_ipc_invocation_rollback_receive(core_);
}

bool invocation_state::finish_dispatch() { return core_ != nullptr && naos_ipc_invocation_finish_dispatch(core_) != 0; }

void invocation_state::set_queue_owner(const handle_t<protocol_state> &owner)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (core_ != nullptr && naos_ipc_invocation_signals(core_) == 0)
        queue_owner_ = owner;
}

void invocation_state::clear_queue_owner()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    queue_owner_.reset();
}

bool invocation_state::cancellation_requested() const
{
    return core_ != nullptr && naos_ipc_invocation_cancellation_requested(core_) != 0;
}

bool invocation_state::execution_interrupted() const
{
    return core_ != nullptr && naos_ipc_invocation_execution_interrupted(core_) != 0;
}

void invocation_state::set_execution_wait_queue(task::wait_queue_t *queue)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    execution_wait_queue_ = queue;
}

void invocation_state::clear_execution_wait_queue(task::wait_queue_t *queue)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (execution_wait_queue_ == queue)
        execution_wait_queue_ = nullptr;
}

void invocation_state::mark_dispatched()
{
    if (core_ != nullptr)
        naos_ipc_invocation_mark_dispatched(core_);
}

bool invocation_state::remove_queued_from_core()
{
    handle_t<protocol_state> owner;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        owner = queue_owner_;
    }
    if (!owner)
        return false;
    auto *request = owner->remove_queued(this);
    if (request == nullptr)
        return false;
    clear_queue_owner();
    memory::Delete<>(memory::KernelCommonAllocatorV, request);
    return true;
}

void invocation_state::notify_core_waiters()
{
    wait_queue_.do_wake_up();
    notify_readiness();
}

void invocation_state::notify_readiness()
{
    // Keep the state lock while dereferencing both registrations.  Their
    // destructors clear the same slots under this lock, so a copied pointer
    // used after unlock would be a use-after-free window.
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (invocation_object_ != nullptr)
        invocation_object_->notify_readiness();
    if (responder_object_ != nullptr)
        responder_object_->notify_readiness();
}

void invocation_state::invocation_object_created(invocation_object *object)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    invocation_object_ = object;
}

void invocation_state::invocation_object_destroyed(invocation_object *object)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (invocation_object_ == object)
        invocation_object_ = nullptr;
}

void invocation_state::responder_object_created(responder_object *object)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    responder_object_ = object;
}

void invocation_state::responder_object_destroyed(responder_object *object)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (responder_object_ == object)
        responder_object_ = nullptr;
}

void invocation_state::wake_core_execution()
{
    task::wait_queue_t *queue = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        queue = execution_wait_queue_;
    }
    if (queue != nullptr)
        queue->do_wake_up();
}

bool invocation_state::cancel(protocol_state *queue_owner)
{
    (void)queue_owner;
    const bool canceled = core_ != nullptr && naos_ipc_invocation_cancel(core_) != 0;
    if (canceled && (signals() & NA_SIGNAL_COMPLETED) != 0)
        disarm_deadline();
    return canceled;
}

na_status_t invocation_state::arm_deadline(const handle_t<invocation_state> &self)
{
    if (operation_deadline() == no_deadline)
        return NA_STATUS_OK;
    auto *watch = memory::New<deadline_watch>(memory::KernelCommonAllocatorV, self);
    if (watch == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    uctx::UninterruptibleContext guard;
    const auto watcher =
        timer::schedule_at(operation_deadline(), timer::timer_handler::bind<&deadline_watch::invoke>(*watch));
    if (watcher == timer::invalid_watcher_id)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, watch);
        expire_deadline();
    }
    else
    {
        uctx::RawSpinLockUninterruptibleContext state_guard(lock_);
        deadline_watcher_ = watcher;
        deadline_watch_ = watch;
    }
    return NA_STATUS_OK;
}

void invocation_state::expire_deadline()
{
    if (core_ != nullptr)
        naos_ipc_invocation_expire_if_due(core_);
}

void invocation_state::close_client()
{
    if (core_ != nullptr)
        naos_ipc_invocation_close_client(core_);
    if ((signals() & NA_SIGNAL_COMPLETED) != 0)
        disarm_deadline();
}

void invocation_state::abandon_responder()
{
    if (core_ != nullptr)
        naos_ipc_invocation_abandon_responder(core_);
    if ((signals() & NA_SIGNAL_COMPLETED) != 0)
        disarm_deadline();
}

bool invocation_state::consume_responder()
{
    return core_ != nullptr && naos_ipc_invocation_consume_responder(core_) != 0;
}

bool invocation_state::reserve_result_budget()
{
    return core_ != nullptr && naos_ipc_invocation_reserve_result_budget(core_) != 0;
}

bool invocation_state::complete_reply(freelibcxx::vector<byte> &bytes, capability::transfer_record_list &resources,
                                      i64 protocol_error, task::resource_table_t *source_resources)
{
    if (core_ == nullptr || !response_within_limits(bytes.size(), resources.size()))
        return false;
    freelibcxx::vector<naos_ipc_resource_t> native_resources(memory::KernelCommonAllocatorV);
    if (!make_invocation_resource_batch(resources, native_resources))
        return false;
    const int completed =
        naos_ipc_invocation_complete_reply(core_, reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(),
                                           native_resources.data(), native_resources.size(), protocol_error);
    if (completed == 0)
    {
        restore_invocation_resource_batch(resources, native_resources);
        return false;
    }
    if (source_resources != nullptr)
        source_resources->commit_native_batch(resources);
    bytes.clear();
    resources.clear();
    disarm_deadline();
    return true;
}

bool invocation_state::complete_reply(freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources,
                                      i64 protocol_error, task::resource_table_t *source_resources)
{
    return complete_reply(bytes, resources, protocol_error, source_resources);
}

bool invocation_state::complete_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason, i64 protocol_error)
{
    const bool completed =
        core_ != nullptr && naos_ipc_invocation_complete_failure(core_, static_cast<u32>(outcome),
                                                                 static_cast<u32>(reason), protocol_error) != 0;
    if (completed)
        disarm_deadline();
    return completed;
}

bool invocation_state::complete_not_delivered(na_outcome_reason_t reason)
{
    const bool completed =
        core_ != nullptr && naos_ipc_invocation_complete_not_delivered(core_, static_cast<u32>(reason)) != 0;
    if (completed)
        disarm_deadline();
    return completed;
}

bool invocation_state::deadline_expired(bool)
{
    if (core_ == nullptr)
        return false;
    naos_ipc_invocation_expire_if_due(core_);
    const auto state_signals = naos_ipc_invocation_signals(core_);
    const bool completed = (state_signals & NA_SIGNAL_COMPLETED) != 0;
    if (completed)
        disarm_deadline();
    return completed;
}

bool invocation_state::response_within_limits(u64 bytes, u64 resources) const
{
    return core_ != nullptr && naos_ipc_invocation_response_within_limits(core_, bytes, resources) != 0;
}

na_status_t invocation_state::claim_result(na_result_frame_t &frame, freelibcxx::vector<byte> &bytes,
                                           capability::transfer_record_list &records)
{
    if (core_ == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    naos_ipc_invocation_result_info_t info{};
    const auto claim_status = static_cast<na_status_t>(
        naos_ipc_invocation_claim_result(core_, frame.byte_capacity, frame.resource_capacity, &info));
    frame.method_id = info.method_id;
    frame.actual_bytes = info.actual_bytes;
    frame.actual_resources = info.actual_resources;
    frame.required_bytes = info.required_bytes;
    frame.required_resources = info.required_resources;
    frame.execution_outcome = info.execution_outcome;
    frame.outcome_reason = info.outcome_reason;
    frame.protocol_error = info.protocol_error;
    if (claim_status != NA_STATUS_OK)
        return claim_status;

    std::uint8_t *detached_bytes = nullptr;
    std::uint64_t detached_byte_count = 0;
    auto status =
        static_cast<na_status_t>(naos_ipc_invocation_take_result_bytes(core_, &detached_bytes, &detached_byte_count));
    if (status != NA_STATUS_OK)
        return status;
    if (detached_byte_count != 0)
    {
        bytes.resize(detached_byte_count, byte{});
        if (bytes.data() == nullptr)
        {
            naos_ipc_invocation_restore_result(core_, nullptr, 0, nullptr, 0);
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        memcpy(bytes.data(), detached_bytes, detached_byte_count);
    }

    freelibcxx::vector<naos_ipc_resource_t> native_resources(memory::KernelCommonAllocatorV);
    native_resources.ensure(info.actual_resources);
    if (info.actual_resources != 0 && native_resources.data() == nullptr)
    {
        naos_ipc_invocation_restore_result(core_, nullptr, 0, nullptr, 0);
        bytes.clear();
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    for (u64 index = 0; index < info.actual_resources; index++)
    {
        naos_ipc_resource_t resource{};
        status = static_cast<na_status_t>(naos_ipc_invocation_take_result_resource(core_, index, &resource));
        if (status != NA_STATUS_OK)
        {
            naos_ipc_invocation_restore_result(core_, nullptr, 0, native_resources.data(), native_resources.size());
            bytes.clear();
            return status;
        }
        native_resources.push_back(resource);
    }

    records.ensure(info.actual_resources);
    if (info.actual_resources != 0 && records.data() == nullptr)
    {
        naos_ipc_invocation_restore_result(core_, nullptr, 0, native_resources.data(), native_resources.size());
        bytes.clear();
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    for (u64 index = 0; index < info.actual_resources; index++)
    {
        auto *holder = static_cast<invocation_resource_holder *>(native_resources[index].value);
        if (holder == nullptr)
        {
            naos_ipc_invocation_restore_result(core_, nullptr, 0, native_resources.data(), native_resources.size());
            bytes.clear();
            return NA_STATUS_IO_ERROR;
        }
        clear_invocation_resource(native_resources[index]);
        records.push_back(capability::transfer_record(NA_HANDLE_INVALID, false, std::move(holder->resource)));
        memory::Delete<>(memory::KernelCommonAllocatorV, holder);
    }
    return NA_STATUS_OK;
}

void invocation_state::restore_result(freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources)
{
    if (core_ == nullptr)
        return;
    freelibcxx::vector<naos_ipc_resource_t> native_resources(memory::KernelCommonAllocatorV);
    if (!make_invocation_resource_batch(resources, native_resources))
        return;
    const auto status =
        naos_ipc_invocation_restore_result(core_, nullptr, 0, native_resources.data(), native_resources.size());
    if (status != NA_STATUS_OK)
        restore_invocation_resource_batch(resources, native_resources);
    else
        resources.clear();
    bytes.clear();
}

na_status_t invocation_state::commit_result()
{
    return core_ == nullptr ? NA_STATUS_RESOURCE_EXHAUSTED
                            : static_cast<na_status_t>(naos_ipc_invocation_commit_result(core_));
}

invocation_object::invocation_object(handle_t<invocation_state> state)
    : kobject(type_e::invocation)
    , state_(std::move(state))
{
    if (state_)
        state_->invocation_object_created(this);
}

invocation_object::~invocation_object()
{
    if (state_)
        state_->invocation_object_destroyed(this);
    if (state_)
        state_->close_client();
}

void invocation_object::on_capability_release(capability::location where)
{
    if (where == capability::location::table_root && state_)
        state_->close_client();
}

na_signal_t invocation_object::capability_signals() const { return state_ ? state_->signals() : 0; }

u64 invocation_object::capability_state() const { return state_ ? static_cast<u64>(state_->signals()) : 0; }

responder_object::responder_object(handle_t<invocation_state> state)
    : kobject(type_e::responder)
    , state_(std::move(state))
    , consumed_(false)
{
    if (state_)
        state_->responder_object_created(this);
}

responder_object::~responder_object()
{
    if (state_)
        state_->responder_object_destroyed(this);
    if (!consumed_ && state_)
        state_->abandon_responder();
}

void responder_object::on_capability_release(capability::location where)
{
    if (where == capability::location::table_root && !consumed_ && state_)
        state_->abandon_responder();
}

na_signal_t responder_object::capability_signals() const { return state_ ? state_->signals() : 0; }

u64 responder_object::capability_state() const { return state_ ? static_cast<u64>(state_->signals()) : 0; }

} // namespace naos::ipc
