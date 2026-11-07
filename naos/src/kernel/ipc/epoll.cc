#include "kernel/ipc/epoll.hpp"

#include "kernel/ipc/channel.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/resource.hpp"
#include "kernel/ucontext.hpp"

#include <utility>

namespace naos::ipc
{
constexpr u64 event_mask = NA_EPOLL_EVENT_READABLE | NA_EPOLL_EVENT_WRITABLE;
constexpr u64 allowed_registration_bits =
    event_mask | NA_EPOLL_EVENT_ERROR | NA_EPOLL_EVENT_HANGUP | NA_EPOLL_EVENT_EDGE_TRIGGERED;

u64 epoll::observed_events(const khandle &target)
{
    if (!target)
        return NA_EPOLL_EVENT_ERROR | NA_EPOLL_EVENT_HANGUP;
    const auto signals = target->capability_signals();
    u64 events = 0;
    if ((signals & (NA_SIGNAL_READABLE | NA_SIGNAL_COMPLETED)) != 0)
        events |= NA_EPOLL_EVENT_READABLE;
    if ((signals & NA_SIGNAL_WRITABLE) != 0)
        events |= NA_EPOLL_EVENT_WRITABLE;
    if ((signals & NA_SIGNAL_PEER_CLOSED) != 0)
        events |= NA_EPOLL_EVENT_HANGUP;
    if ((signals & NA_SIGNAL_OBJECT_REVOKED) != 0)
        events |= NA_EPOLL_EVENT_ERROR | NA_EPOLL_EVENT_HANGUP;
    if ((signals & NA_SIGNAL_CANCEL_REQUESTED) != 0)
        events |= NA_EPOLL_EVENT_ERROR;
    return events;
}

u64 epoll::visible_events(const registration &registration, u64 observed)
{
    const u64 requested = registration.event.events;
    return (observed & event_mask & requested) | (observed & (NA_EPOLL_EVENT_ERROR | NA_EPOLL_EVENT_HANGUP));
}

bool epoll::target_events(const registration &registration, u64 &events)
{
    events = observed_events(registration.target);
    return true;
}

epoll::epoll()
    : kobject(type_e::epoll)
    , registration_chunks_(memory::MemoryAllocatorV)
{
    // Keep only the chunk directory and first slot chunk resident initially.
    // Further chunks are allocated by EPOLL_CTL_ADD before taking lock_.
    registration_chunks_.ensure(max_registration_chunks);
    if (registration_chunks_.capacity() < max_registration_chunks)
        return;
    registration_chunks_.expand(max_registration_chunks, static_cast<registration_chunk *>(nullptr));
    registration_chunk *initial_chunk = allocate_registration_chunk();
    if (initial_chunk == nullptr)
        return;
    registration_chunks_[0] = initial_chunk;
    registration_chunk_count_ = 1;
    registration_count_ = initial_chunk->size();
    initialized_ = registration_count_ != 0;
}

epoll::~epoll()
{
    freelibcxx::vector<khandle> targets(memory::KernelCommonAllocatorV);
    freelibcxx::vector<u64> tokens(memory::KernelCommonAllocatorV);
    // Reserve before taking the non-sleepable epoll lock.  The destructor is
    // the last owner, so the registration count cannot grow here.
    const u64 registration_count = registration_count_;
    targets.ensure(registration_count);
    tokens.ensure(registration_count);
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 index = 0; index < registration_count_; index++)
        {
            registration &slot = registration_at_locked(index);
            if (!slot.in_use)
                continue;
            targets.push_back(slot.target);
            tokens.push_back(slot.watcher_token);
            slot.in_use = false;
            slot.queued = false;
            slot.retiring = false;
        }
        ready_head_ = invalid_slot;
        ready_tail_ = invalid_slot;
        ready_count_ = 0;
    }
    for (u64 index = 0; index < targets.size(); index++)
    {
        if (targets[index])
            targets[index]->remove_readiness_watcher(this, tokens[index]);
    }
    for (u64 index = 0; index < registration_chunk_count_; index++)
        memory::MemoryAllocatorV->Delete(registration_chunks_[index]);
    registration_chunks_.clear();
    wait_queue_.do_wake_up();
}

void epoll::notify() { wait_queue_.do_wake_up(); }

u64 epoll::find_registration_locked(na_handle_t handle) const
{
    for (u64 index = 0; index < registration_count_; index++)
    {
        const registration &slot = registration_at_locked(index);
        if (slot.in_use && slot.handle == handle)
            return index;
    }
    return invalid_slot;
}

epoll::registration &epoll::registration_at_locked(u64 index)
{
    CXXASSERT(index < registration_count_);
    const u64 chunk_index = index / registration_chunk_size;
    const u64 slot_index = index % registration_chunk_size;
    return (*registration_chunks_.at(chunk_index))[slot_index];
}

const epoll::registration &epoll::registration_at_locked(u64 index) const
{
    CXXASSERT(index < registration_count_);
    const u64 chunk_index = index / registration_chunk_size;
    const u64 slot_index = index % registration_chunk_size;
    return (*registration_chunks_.at(chunk_index))[slot_index];
}

bool epoll::has_free_registration_locked() const
{
    for (u64 index = 0; index < registration_count_; index++)
    {
        const registration &slot = registration_at_locked(index);
        if (!slot.in_use && !slot.retiring)
            return true;
    }
    return false;
}

epoll::registration_chunk *epoll::allocate_registration_chunk()
{
    auto *chunk = memory::MemoryAllocatorV->New<registration_chunk>(memory::MemoryAllocatorV);
    if (chunk == nullptr)
        return nullptr;
    chunk->ensure(registration_chunk_size);
    if (chunk->capacity() < registration_chunk_size)
    {
        memory::MemoryAllocatorV->Delete(chunk);
        return nullptr;
    }
    chunk->expand(registration_chunk_size, registration{});
    return chunk;
}

bool epoll::ensure_registration_capacity(na_handle_t target)
{
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (find_registration_locked(target) != invalid_slot || has_free_registration_locked())
            return true;
        if (registration_count_ >= max_registrations)
            return false;
    }

    registration_chunk *candidate = allocate_registration_chunk();
    if (candidate == nullptr)
        return false;

    bool available = false;
    bool attached = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (find_registration_locked(target) != invalid_slot || has_free_registration_locked())
        {
            available = true;
        }
        else if (registration_count_ < max_registrations && registration_chunk_count_ < max_registration_chunks)
        {
            registration_chunks_[registration_chunk_count_++] = candidate;
            registration_count_ += registration_chunk_size;
            if (registration_count_ > max_registrations)
                registration_count_ = max_registrations;
            attached = true;
            available = true;
        }
    }

    if (!attached)
        memory::MemoryAllocatorV->Delete(candidate);
    return available;
}

void epoll::queue_ready_locked(u64 token)
{
    CXXASSERT(token < registration_count_);
    registration &slot = registration_at_locked(token);
    CXXASSERT(slot.queued);
    CXXASSERT(ready_count_ < registration_count_);
    slot.ready_previous = ready_tail_;
    slot.ready_next = invalid_slot;
    if (ready_tail_ == invalid_slot)
        ready_head_ = token;
    else
        registration_at_locked(ready_tail_).ready_next = token;
    ready_tail_ = token;
    ready_count_++;
}

void epoll::unqueue_ready_locked(u64 token)
{
    if (token >= registration_count_)
        return;
    registration &slot = registration_at_locked(token);
    if (!slot.queued)
        return;
    if (slot.ready_previous == invalid_slot)
        ready_head_ = slot.ready_next;
    else
        registration_at_locked(slot.ready_previous).ready_next = slot.ready_next;
    if (slot.ready_next == invalid_slot)
        ready_tail_ = slot.ready_previous;
    else
        registration_at_locked(slot.ready_next).ready_previous = slot.ready_previous;
    slot.ready_previous = invalid_slot;
    slot.ready_next = invalid_slot;
    slot.queued = false;
    CXXASSERT(ready_count_ != 0);
    ready_count_--;
}

bool epoll::take_ready_locked(u64 &token, registration &snapshot)
{
    if (ready_count_ == 0)
        return false;
    token = ready_head_;
    unqueue_ready_locked(token);
    if (token < registration_count_)
    {
        registration &slot = registration_at_locked(token);
        if (slot.in_use)
        {
            snapshot = slot;
            return true;
        }
    }
    // An entry for a slot that was freed (or reused between the notify and this
    // drain) carries no state; report it as consumed but empty.
    token = invalid_slot;
    return true;
}

void epoll::on_readiness_change(u64 token)
{
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 index = 0; index < registration_count_; index++)
        {
            registration &slot = registration_at_locked(index);
            if (slot.in_use && slot.watcher_token == token && !slot.queued)
            {
                slot.queued = true;
                queue_ready_locked(index);
                break;
            }
        }
    }
    // Wake outside the lock: the notifier holds the target's readiness lock and
    // do_wake_up takes the wait queue lock, which must not nest inside lock_.
    wait_queue_.do_wake_up();
}

na_status_t epoll::control(task::resource_table_t &resources, na_handle_t target, u32 operation,
                           const na_epoll_event_t *event)
{
    if (target == NA_HANDLE_INVALID)
        return NA_STATUS_INVALID_HANDLE;
    if (!initialized_)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    // Control operations may allocate a new slot chunk. Serialize them so
    // concurrent ADD calls cannot consume the same final free slot between
    // the capacity check and registration insertion.
    uctx::LockGuard_t<lock::mutex_t> control_guard(registration_growth_lock_);

    capability::entry target_entry;
    if (!resources.lookup_native(target, target_entry) || !target_entry.object)
    {
        // DEL is allowed after the target capability has already been closed;
        // the registration itself is still removable by its handle number.
        if (operation != NA_EPOLL_CTL_DEL)
            return NA_STATUS_INVALID_HANDLE;
    }
    else if ((target_entry.meta.meta_rights & NA_RIGHT_WAIT) == 0)
    {
        return NA_STATUS_ACCESS_DENIED;
    }
    else if (target_entry.object->is<epoll>())
    {
        return NA_STATUS_INVALID_ARGUMENT;
    }

    if (operation == NA_EPOLL_CTL_DEL)
    {
        u64 token = invalid_slot;
        khandle removed_target;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            const u64 index = find_registration_locked(target);
            if (index != invalid_slot)
            {
                registration &slot = registration_at_locked(index);
                removed_target = slot.target;
                if (slot.queued)
                    unqueue_ready_locked(index);
                slot.in_use = false;
                slot.queued = false;
                slot.retiring = true;
                // Drop the slot's own reference while the local copy keeps the
                // object alive, so this cannot run a destructor under lock_.
                slot.target.reset();
                token = index;
            }
        }
        if (token == invalid_slot)
            return NA_STATUS_INVALID_HANDLE;
        if (removed_target)
            removed_target->remove_readiness_watcher(this, token);
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            registration_at_locked(token).retiring = false;
        }
        notify();
        return NA_STATUS_OK;
    }

    if (event == nullptr || event->events == 0 || (event->events & ~allowed_registration_bits) != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (operation != NA_EPOLL_CTL_ADD && operation != NA_EPOLL_CTL_MOD)
        return NA_STATUS_INVALID_ARGUMENT;
    if (operation == NA_EPOLL_CTL_ADD && !ensure_registration_capacity(target))
        return NA_STATUS_RESOURCE_EXHAUSTED;

    na_status_t result = NA_STATUS_OK;
    khandle added_target;
    u64 added_slot = invalid_slot;
    u64 added_token = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const u64 index = find_registration_locked(target);
        if (index != invalid_slot)
        {
            registration &slot = registration_at_locked(index);
            if (operation == NA_EPOLL_CTL_ADD)
                result = NA_STATUS_ALREADY_CONSUMED;
            else if (!target_entry.object || slot.generation != target_entry.generation)
                result = NA_STATUS_INVALID_HANDLE;
            else
            {
                slot.event = *event;
                slot.last_ready = 0;
                slot.last_readiness_epoch = slot.target ? slot.target->readiness_epoch() : 0;
                // A MOD invalidates the previous edge state, so re-examine the
                // slot even without a new target-side notification.
                if (!slot.queued)
                {
                    slot.queued = true;
                    queue_ready_locked(index);
                }
            }
        }
        else if (operation == NA_EPOLL_CTL_MOD || !target_entry.object)
        {
            result = NA_STATUS_INVALID_HANDLE;
        }
        else
        {
            // Reuse a fully retired slot so repeated add/remove cycles do not
            // grow the table; otherwise append a new stable index.
            for (u64 candidate = 0; candidate < registration_count_; candidate++)
            {
                const registration &slot = registration_at_locked(candidate);
                if (!slot.in_use && !slot.retiring)
                {
                    added_slot = candidate;
                    break;
                }
            }
            if (added_slot == invalid_slot)
            {
                if (registration_count_ >= max_registrations)
                {
                    result = NA_STATUS_RESOURCE_EXHAUSTED;
                }
                else
                {
                    // Capacity is prepared outside lock_ before entering this
                    // registration transaction.
                    CXXASSERT(false);
                }
            }
            if (result == NA_STATUS_OK)
            {
                added_target = target_entry.object;
                registration slot{};
                slot.handle = target;
                slot.watcher_token = next_watcher_token_++;
                if (slot.watcher_token == 0)
                    slot.watcher_token = next_watcher_token_++;
                added_token = slot.watcher_token;
                slot.generation = target_entry.generation;
                slot.event = *event;
                slot.last_ready = 0;
                slot.last_readiness_epoch = added_target->readiness_epoch();
                slot.target = added_target;
                slot.in_use = true;
                slot.queued = false;
                slot.retiring = false;
                registration_at_locked(added_slot) = std::move(slot);

                // Queue links live in the stable slot, so enqueue cannot
                // allocate or overflow.
                registration_at_locked(added_slot).queued = true;
                queue_ready_locked(added_slot);
            }
        }
    }
    if (result != NA_STATUS_OK)
        return result;
    if (added_target)
    {
        auto *added_object = added_target.operator->();
        if (!added_target->add_readiness_watcher(this, added_token))
        {
            // The registration was made visible before the target-side
            // watcher could be installed.  Roll it back without invoking an
            // allocator while lock_ is held; the local handle keeps the
            // target object alive until the rollback is complete.
            {
                uctx::RawSpinLockUninterruptibleContext guard(lock_);
                if (added_slot < registration_count_ && registration_at_locked(added_slot).in_use &&
                    registration_at_locked(added_slot).watcher_token == added_token &&
                    registration_at_locked(added_slot).target.operator->() == added_object)
                {
                    if (registration_at_locked(added_slot).queued)
                        unqueue_ready_locked(added_slot);
                    registration_at_locked(added_slot).target.reset();
                    registration_at_locked(added_slot).in_use = false;
                    registration_at_locked(added_slot).queued = false;
                    registration_at_locked(added_slot).retiring = false;
                }
            }
            notify();
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }

        // DEL may have retired this slot while the target-side watcher was
        // being installed.  In that interleaving DEL cannot remove a watcher
        // that did not exist yet, so verify the registration after the
        // installation and undo it when the slot no longer names this target.
        // A later ADD may already have reused the slot; compare the target
        // pointer as well as the slot state before treating it as ours.
        bool still_registered = false;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            if (added_slot < registration_count_)
            {
                const auto &slot = registration_at_locked(added_slot);
                still_registered = slot.in_use && !slot.retiring && slot.watcher_token == added_token &&
                                   slot.target.operator->() == added_object;
            }
        }
        if (!still_registered)
            added_target->remove_readiness_watcher(this, added_token);
    }
    notify();
    return NA_STATUS_OK;
}

bool epoll::process_ready(freelibcxx::vector<na_epoll_event_t> *events)
{
    const bool predicate = events == nullptr;
    bool produced_any = false;
    u64 drain_count = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        drain_count = ready_count_;
    }
    // Only slots queued at entry are examined.  Slots re-queued while visible
    // (level-triggered) are appended behind the drain barrier, so the same slot
    // cannot be reported twice within one collection.
    for (u64 processed = 0; processed < drain_count; processed++)
    {
        if (events != nullptr && events->size() == events->capacity())
            break;

        u64 token = 0;
        registration snapshot{};
        bool have = false;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            have = take_ready_locked(token, snapshot);
        }
        if (!have)
            break;
        if (token == invalid_slot)
            continue;

        // No epoll lock is held here: capability_signals() and
        // readiness_epoch() take target locks, and a notifier may call
        // on_readiness_change() concurrently.
        u64 observed = 0;
        (void)target_events(snapshot, observed);
        const u64 visible = visible_events(snapshot, observed);
        const bool edge_triggered = (snapshot.event.events & NA_EPOLL_EVENT_EDGE_TRIGGERED) != 0;

        u64 report = 0;
        u64 next_last_ready = snapshot.last_ready;
        u64 next_last_epoch = snapshot.last_readiness_epoch;
        bool requeue = false;

        if (edge_triggered)
        {
            const u64 readiness_epoch = snapshot.target ? snapshot.target->readiness_epoch() : 0;
            const u64 rising = visible & ~snapshot.last_ready;
            if (visible == 0)
            {
                next_last_ready = 0;
                next_last_epoch = readiness_epoch;
            }
            else if (rising == 0 && readiness_epoch == snapshot.last_readiness_epoch)
            {
                // Nothing new since the last report.
            }
            else
            {
                report = rising == 0 ? visible : rising;
                next_last_ready = visible;
                next_last_epoch = readiness_epoch;
            }
        }
        else if (visible != 0)
        {
            report = visible;
            // Level-triggered registrations stay queued while visible so a
            // later wait reports them again without a new notification.
            requeue = true;
        }

        if (predicate && report != 0)
        {
            // The wait predicate must leave the evidence in place: the
            // collection that follows a successful wait has to observe the same
            // rising edge, so its bookkeeping must not advance here.
            requeue = true;
            next_last_ready = snapshot.last_ready;
            next_last_epoch = snapshot.last_readiness_epoch;
        }

        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            if (token < registration_count_)
            {
                registration &slot = registration_at_locked(token);
                if (slot.in_use && slot.watcher_token == snapshot.watcher_token && slot.handle == snapshot.handle &&
                    slot.generation == snapshot.generation)
                {
                    slot.last_ready = next_last_ready;
                    slot.last_readiness_epoch = next_last_epoch;
                    if (requeue && !slot.queued)
                    {
                        slot.queued = true;
                        queue_ready_locked(token);
                    }
                }
            }
        }

        if (report == 0)
            continue;

        produced_any = true;
        if (predicate)
            return true;
        events->push_back(na_epoll_event_t{report, snapshot.event.data});
    }
    return produced_any;
}

bool epoll::has_ready() { return process_ready(nullptr); }

void epoll::collect_ready(freelibcxx::vector<na_epoll_event_t> &events) { (void)process_ready(&events); }

na_status_t epoll::wait(freelibcxx::vector<na_epoll_event_t> &events, timeclock::microsecond_t deadline,
                        freelibcxx::function_ref<void()> exit_cleanup)
{
    for (;;)
    {
        events.clear();
        collect_ready(events);
        if (!events.empty())
            return NA_STATUS_OK;

        const auto status = wait_for_condition(wait_queue_, [&] { return has_ready(); }, deadline, exit_cleanup);
        if (status != NA_STATUS_OK)
            return status;

        // The wait predicate only establishes that a transition occurred.  A
        // second collection is required after waking; otherwise the syscall
        // can return OK with an empty event array and leave the reactor asleep
        // until an unrelated transition happens.
    }
}

na_signal_t epoll::capability_signals() const
{
    const bool ready = const_cast<epoll *>(this)->has_ready();
    return ready ? NA_SIGNAL_READABLE : 0;
}

} // namespace naos::ipc
