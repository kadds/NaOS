#pragma once

#include <atomic>

namespace task
{
enum class thread_lifecycle_phase : unsigned char
{
    live,
    exiting,
    exited,
    reaping,
};

class thread_lifecycle
{
  public:
    bool try_begin_exit() { return transition(thread_lifecycle_phase::live, thread_lifecycle_phase::exiting); }
    // Exit work allocation may fail before scheduler removal was submitted.
    bool cancel_exit() { return transition(thread_lifecycle_phase::exiting, thread_lifecycle_phase::live); }
    bool complete_exit() { return transition(thread_lifecycle_phase::exiting, thread_lifecycle_phase::exited); }
    // Only the creator/abort owner may discard a thread never given to a CPU.
    bool discard_unstarted() { return transition(thread_lifecycle_phase::live, thread_lifecycle_phase::exited); }
    bool try_begin_reap() { return transition(thread_lifecycle_phase::exited, thread_lifecycle_phase::reaping); }

    thread_lifecycle_phase phase() const { return phase_.load(std::memory_order_acquire); }
    bool started() const { return phase() != thread_lifecycle_phase::live; }
    bool can_reap() const { return phase() == thread_lifecycle_phase::exited; }
    bool is_reaping() const { return phase() == thread_lifecycle_phase::reaping; }

  private:
    std::atomic<thread_lifecycle_phase> phase_{thread_lifecycle_phase::live};

    bool transition(thread_lifecycle_phase expected, thread_lifecycle_phase desired)
    {
        return phase_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    }
};
} // namespace task
