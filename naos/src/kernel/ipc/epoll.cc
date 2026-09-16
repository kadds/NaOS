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
    , registrations_(memory::MemoryAllocatorV)
    , ready_ring_(memory::MemoryAllocatorV)
{
}

epoll::~epoll()
{
    freelibcxx::vector<khandle> targets(memory::KernelCommonAllocatorV);
    freelibcxx::vector<u64> tokens(memory::KernelCommonAllocatorV);
    // Reserve before taking the non-sleepable epoll lock.  The destructor is
    // the last owner, so the registration count cannot grow here.
    const u64 registration_count = registrations_.size();
    targets.ensure(registration_count);
    tokens.ensure(registration_count);
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 index = 0; index < registrations_.size(); index++)
        {
            registration &slot = registrations_[index];
            if (!slot.in_use)
                continue;
            targets.push_back(slot.target);
            tokens.push_back(slot.watcher_token);
            slot.in_use = false;
            slot.queued = false;
            slot.retiring = false;
        }
        registrations_.clear();
        ready_ring_.clear();
        ready_head_ = 0;
        ready_count_ = 0;
    }
    for (u64 index = 0; index < targets.size(); index++)
    {
        if (targets[index])
            targets[index]->remove_readiness_watcher(this, tokens[index]);
    }
    wait_queue_.do_wake_up();
}

void epoll::notify() { wait_queue_.do_wake_up(); }

u64 epoll::find_registration_locked(na_handle_t handle) const
{
    for (u64 index = 0; index < registrations_.size(); index++)
    {
        if (registrations_.at(index).in_use && registrations_.at(index).handle == handle)
            return index;
    }
    return invalid_slot;
}

void epoll::queue_ready_locked(u64 token)
{
    const u64 size = ready_ring_.size();
    CXXASSERT(ready_count_ < size);
    ready_ring_[(ready_head_ + ready_count_) % size] = token;
    ready_count_++;
}

void epoll::unqueue_ready_locked(u64 token)
{
    const u64 size = ready_ring_.size();
    u64 position = invalid_slot;
    for (u64 index = 0; index < ready_count_; index++)
    {
        if (ready_ring_[(ready_head_ + index) % size] == token)
        {
            position = index;
            break;
        }
    }
    if (position == invalid_slot)
        return;
    // Shift logical queue entries in place.  The destination is always behind
    // the source, including at the wrap boundary, so this needs no temporary
    // allocation while lock_ is held.
    for (u64 index = position; index + 1 < ready_count_; index++)
    {
        const u64 destination = (ready_head_ + index) % size;
        const u64 source = (ready_head_ + index + 1) % size;
        ready_ring_[destination] = ready_ring_[source];
    }
    ready_count_--;
}

bool epoll::take_ready_locked(u64 &token, registration &snapshot)
{
    if (ready_count_ == 0)
        return false;
    token = ready_ring_[ready_head_];
    ready_head_ = (ready_head_ + 1) % ready_ring_.size();
    ready_count_--;
    if (token < registrations_.size())
    {
        registration &slot = registrations_[token];
        if (slot.in_use && slot.queued)
        {
            slot.queued = false;
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
        for (u64 index = 0; index < registrations_.size(); index++)
        {
            registration &slot = registrations_[index];
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
                registration &slot = registrations_[index];
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
            registrations_[token].retiring = false;
        }
        notify();
        return NA_STATUS_OK;
    }

    if (event == nullptr || event->events == 0 || (event->events & ~allowed_registration_bits) != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (operation != NA_EPOLL_CTL_ADD && operation != NA_EPOLL_CTL_MOD)
        return NA_STATUS_INVALID_ARGUMENT;

    na_status_t result = NA_STATUS_OK;
    khandle added_target;
    u64 added_slot = invalid_slot;
    u64 added_token = 0;
    if (operation == NA_EPOLL_CTL_ADD)
    {
        // Serialize capacity preparation with the registration commit.  If
        // two ADDs prepared against the same registration count, both could
        // grow the ring by one and the second commit would enqueue into a
        // full ring.
        allocation_lock_.lock();
        u64 required_capacity = 0;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            required_capacity = registrations_.size() + 1;
        }
        // These are the only operations below that may grow a container.  Do
        // them before acquiring lock_; the commit phase then only constructs
        // elements in already allocated storage.
        registrations_.ensure(required_capacity);
        ready_ring_.expand(required_capacity, 0);
        const bool capacity_ready =
            registrations_.capacity() >= required_capacity && ready_ring_.size() >= required_capacity;
        if (!capacity_ready)
        {
            allocation_lock_.unlock();
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }
    }
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const u64 index = find_registration_locked(target);
        if (index != invalid_slot)
        {
            registration &slot = registrations_[index];
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
            // Reuse a fully retired slot so repeated add/remove cycles do not
            // grow the table; otherwise append a new stable index.
            for (u64 candidate = 0; candidate < registrations_.size(); candidate++)
            {
                if (!registrations_[candidate].in_use && !registrations_[candidate].retiring)
                {
                    added_slot = candidate;
                    break;
                }
            }
            if (added_slot == invalid_slot)
            {
                registrations_.push_back(std::move(slot));
                added_slot = registrations_.size() - 1;
            }
            else
            {
                registrations_[added_slot] = std::move(slot);
            }
            // The target may already be ready; queue the new slot so the first
            // wait observes it without waiting for a fresh notification.
            registrations_[added_slot].queued = true;
            queue_ready_locked(added_slot);
        }
    }
    if (operation == NA_EPOLL_CTL_ADD)
        allocation_lock_.unlock();
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
                if (added_slot < registrations_.size() && registrations_[added_slot].in_use &&
                    registrations_[added_slot].watcher_token == added_token &&
                    registrations_[added_slot].target.operator->() == added_object)
                {
                    if (registrations_[added_slot].queued)
                        unqueue_ready_locked(added_slot);
                    registrations_[added_slot].target.reset();
                    registrations_[added_slot].in_use = false;
                    registrations_[added_slot].queued = false;
                    registrations_[added_slot].retiring = false;
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
            if (added_slot < registrations_.size())
            {
                const auto &slot = registrations_[added_slot];
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
            if (token < registrations_.size())
            {
                registration &slot = registrations_[token];
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

na_status_t epoll::wait(freelibcxx::vector<na_epoll_event_t> &events, timeclock::microsecond_t deadline)
{
    for (;;)
    {
        events.clear();
        collect_ready(events);
        if (!events.empty())
            return NA_STATUS_OK;

        const auto status = wait_for_condition(wait_queue_, [&] { return has_ready(); }, deadline);
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
