#pragma once

#include "kernel/arch/cpu.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/lock.hpp"

namespace naos::ipc
{

/// Lock adapter used when the transport-neutral IPC core runs in the kernel.
/// Syscall entry enables interrupts before dispatch, so a timer interrupt must
/// not preempt a thread while it owns the core lock.  The saved state is
/// per-CPU because a different CPU may acquire the lock after release.
class core_lock
{
  public:
    core_lock() = default;
    core_lock(const core_lock &) = delete;
    core_lock &operator=(const core_lock &) = delete;

    void acquire()
    {
        const auto cpu = arch::cpu::id();
        irq_enabled_[cpu] = arch::idt::save_and_disable();
        lock_.lock();
    }

    void release()
    {
        const auto cpu = arch::cpu::id();
        const bool restore_irq = irq_enabled_[cpu];
        lock_.unlock();
        if (restore_irq)
            arch::idt::enable();
    }

  private:
    lock::spinlock_t lock_;
    bool irq_enabled_[arch::cpu::max_cpu_support]{};
};

inline void core_lock_acquire(void *context) { static_cast<core_lock *>(context)->acquire(); }

inline void core_lock_release(void *context) { static_cast<core_lock *>(context)->release(); }

} // namespace naos::ipc
