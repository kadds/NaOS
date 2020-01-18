#pragma once
#include "arch/idt.hpp"
#include "lock.hpp"
#include "preempt.hpp"
#include <type_traits>

namespace uctx
{

struct UninterruptibleController
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

template <typename Controller> struct BaseUninterruptibleController
{
    UninterruptibleController ctl;
    Controller ctl2;
    template <typename... T>
    BaseUninterruptibleController(T &&... args)
        : ctl2(std::forward<T>(args)...)
    {
    }

    void begin()
    {
        ctl.begin();
        ctl2.begin();
    }

    void end()
    {
        ctl2.end();
        ctl.end();
    }
};

struct RawSpinLockController
{
  private:
    lock::spinlock_t &sl;

  public:
    RawSpinLockController(lock::spinlock_t &sl)
        : sl(sl)
    {
    }
    void begin() { sl.lock(); }
    void end() { sl.unlock(); }
};

using RawSpinLockUninterruptibleController = BaseUninterruptibleController<RawSpinLockController>;

struct RawReadLockController
{
  private:
    lock::rw_lock_t &lock;

  public:
    RawReadLockController(lock::rw_lock_t &lock)
        : lock(lock)
    {
    }

    void begin() { lock.lock_read(); }
    void end() { lock.unlock_read(); }
};

using RawReadLockUninterruptibleController = BaseUninterruptibleController<RawReadLockController>;

struct RawWriteLockController
{
  private:
    lock::rw_lock_t &lock;

  public:
    RawWriteLockController(lock::rw_lock_t &lock)
        : lock(lock)
    {
    }

    void begin() { lock.lock_write(); }
    void end() { lock.unlock_write(); }
};

using RawWriteLockUninterruptibleController = BaseUninterruptibleController<RawWriteLockController>;

template <typename Controller> struct Guard_t
{
    Controller ctrl;
    template <typename... T>
    Guard_t(T &&... arg)
        : ctrl(std::forward<T>(arg)...)
    {
        ctrl.begin();
    }

    ~Guard_t() { ctrl.end(); }
};

template <typename Lock> struct LockGuard_t
{
    Lock &ctrl;
    explicit LockGuard_t(Lock &l)
        : ctrl(l)
    {
        ctrl.lock();
    }

    ~LockGuard_t() { ctrl.unlock(); }
};

using RawSpinLockContext = Guard_t<RawSpinLockController>;
using SpinLockContext = Guard_t<RawSpinLockController>;
using UninterruptibleContext = Guard_t<UninterruptibleController>;
using SpinLockUninterruptibleContext = Guard_t<RawSpinLockUninterruptibleController>;
using RawSpinLockUninterruptibleContext = Guard_t<RawSpinLockUninterruptibleController>;
using RawReadLockContext = Guard_t<RawReadLockController>;
using RawWriteLockContext = Guard_t<RawWriteLockController>;
using RawReadLockUninterruptibleContext = Guard_t<RawReadLockUninterruptibleController>;
using RawWriteLockUninterruptibleContext = Guard_t<RawWriteLockUninterruptibleController>;

} // namespace uctx
