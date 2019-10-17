#pragma once
#include "kernel/arch/idt.hpp"
#include "kernel/lock.hpp"
#include "kernel/task.hpp"
namespace uctx
{

struct UnInterruptableContextController
{
  private:
    bool IF;

  public:
    void begin() { IF = arch::idt::save_and_disable(); }
    void end()
    {
        if (IF)
            arch::idt::enable();

        else
            arch::idt::disable();
    }
};

struct UnInterruptableContext
{
  private:
    UnInterruptableContextController ctrl;

  public:
    UnInterruptableContext() { ctrl.begin(); }
    ~UnInterruptableContext() { ctrl.end(); }
};

struct SpinLockContextController
{
  private:
    lock::spinlock_t &sl;

  public:
    SpinLockContextController(lock::spinlock_t &sl)
        : sl(sl)
    {
    }
    void begin() { sl.lock(); }
    void end() { sl.unlock(); }
};

struct SpinLockContext
{
  private:
    SpinLockContextController ctrl;

  public:
    SpinLockContext(lock::spinlock_t &sl)
        : ctrl(sl)
    {
        ctrl.begin();
    }
    ~SpinLockContext() { ctrl.end(); }
};

struct SpinLockUnInterruptableContextController

{
  private:
    SpinLockContextController lock_ctrl;
    UnInterruptableContextController intr;

  public:
    SpinLockUnInterruptableContextController(lock::spinlock_t &sl)
        : lock_ctrl(sl)
    {
    }
    void begin()
    {
        intr.begin();
        lock_ctrl.begin();
    }
    void end()
    {
        lock_ctrl.end();
        intr.end();
    }
};

struct SpinLockUnInterruptableContext
{
  private:
    SpinLockUnInterruptableContextController ctrl;

  public:
    SpinLockUnInterruptableContext(lock::spinlock_t &sl)
        : ctrl(sl)
    {
        ctrl.begin();
    }
    ~SpinLockUnInterruptableContext() { ctrl.end(); }
};

bool inline in_interrupt()
{
    // TODO:
    // task::current()->kernel_stack_high;
    return true;
}

} // namespace uctx
