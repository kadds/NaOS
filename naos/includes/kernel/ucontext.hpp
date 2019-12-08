#pragma once
#include "arch/idt.hpp"
#include "lock.hpp"
#include "preempt.hpp"
namespace uctx
{
struct UnPreemptContextController
{
    void begin() { task::disable_preempt(); }
    void end() { task::enable_preempt(); }
};

struct UnPreemptContext
{
    UnPreemptContextController ctr;
    UnPreemptContext() { ctr.begin(); }
    ~UnPreemptContext() { ctr.end(); }
};

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
    UnPreemptContextController ctx;

  public:
    SpinLockContextController(lock::spinlock_t &sl)
        : sl(sl)
    {
    }
    void begin()
    {
        ctx.begin();
        sl.lock();
    }
    void end()
    {
        sl.unlock();
        ctx.end();
    }
};

struct RawSpinLockContextController
{
  private:
    lock::spinlock_t &sl;

  public:
    RawSpinLockContextController(lock::spinlock_t &sl)
        : sl(sl)
    {
    }
    void begin() { sl.lock(); }
    void end() { sl.unlock(); }
};

struct RawSpinLockContext
{
  private:
    RawSpinLockContextController ctrl;

  public:
    RawSpinLockContext(lock::spinlock_t &sl)
        : ctrl(sl)
    {
        ctrl.begin();
    }
    ~RawSpinLockContext() { ctrl.end(); }
};

struct RawSpinLockUnInterruptableContextController
{
  private:
    lock::spinlock_t &sl;
    UnInterruptableContextController ctrx;

  public:
    RawSpinLockUnInterruptableContextController(lock::spinlock_t &sl)
        : sl(sl)
    {
    }
    void begin()
    {
        ctrx.begin();
        sl.lock();
    }
    void end()
    {
        sl.unlock();
        ctrx.end();
    }
};

struct RawSpinLockUnInterruptableContext
{
  private:
    RawSpinLockUnInterruptableContextController ctrl;

  public:
    RawSpinLockUnInterruptableContext(lock::spinlock_t &sl)
        : ctrl(sl)
    {
        ctrl.begin();
    }
    ~RawSpinLockUnInterruptableContext() { ctrl.end(); }
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

} // namespace uctx
