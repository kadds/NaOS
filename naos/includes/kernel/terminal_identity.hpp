#pragma once

#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"
#include <array>
#include <atomic>

namespace dev::tty
{
/// Kernel-owned identity of one userspace terminal (PTY or console).
/// It carries only the terminal-scoped session/foreground-group state; all
/// line discipline lives in the userspace driver.
class terminal_identity final : public kobject
{
  public:
    static constexpr kobject::type_e type_of() { return kobject::type_e::terminal_identity; }

    terminal_identity(u64 id, u64 generation, u64 mode, const std::array<u8, 16> &token)
        : kobject(kobject::type_e::terminal_identity)
        , id_(id)
        , generation_(generation)
        , mode_(mode)
        , token_(token)
    {
    }

    bool capability_is_unique() const override { return false; }

    u64 id() const { return id_; }
    u64 generation() const { return generation_; }
    u64 mode() const { return mode_; }
    const std::array<u8, 16> &token() const { return token_; }
    bool live() const { return live_.load(); }

    bool matches_locator(u64 id, u64 generation, const u8 *token, u64 token_size) const
    {
        if (!live() || id != id_ || generation != generation_ || token == nullptr || token_size != token_.size())
            return false;
        u8 mismatch = 0;
        for (u64 i = 0; i < token_.size(); i++)
            mismatch |= static_cast<u8>(token_[i] ^ token[i]);
        return mismatch == 0;
    }

    bool revoke() { return revoke_and_take_foreground(nullptr); }

    // Revoke and snapshot the foreground group under one identity lock. The
    // caller may signal the returned group after releasing the lock, but it
    // can never observe a group that was installed after revocation.
    bool revoke_and_take_foreground(group_id *foreground)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!live_.load(std::memory_order_acquire))
            return false;
        if (foreground != nullptr)
            *foreground = foreground_process_group_.load(std::memory_order_relaxed);
        live_.store(false, std::memory_order_release);
        io_lease_count_ = 0;
        return true;
    }

    void set_session_id(::session_id session) { session_id_.store(session); }
    ::session_id session_id() const { return session_id_.load(); }

    void set_foreground_process_group(group_id group)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (live_.load(std::memory_order_relaxed))
            foreground_process_group_.store(group, std::memory_order_release);
    }

    bool try_set_foreground_process_group(group_id group)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!live_.load(std::memory_order_relaxed) || io_lease_count_ != 0)
            return false;
        foreground_process_group_.store(group, std::memory_order_release);
        return true;
    }

    bool try_acquire_io_lease(group_id process_group)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!live_.load(std::memory_order_relaxed))
            return false;
        const auto foreground = foreground_process_group_.load(std::memory_order_relaxed);
        if (foreground != 0 && foreground != process_group)
            return false;
        io_lease_count_++;
        return true;
    }

    bool try_acquire_unrestricted_io_lease()
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!live_.load(std::memory_order_relaxed))
            return false;
        io_lease_count_++;
        return true;
    }

    void release_io_lease()
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (io_lease_count_ != 0)
            io_lease_count_--;
    }
    group_id foreground_process_group() const
    {
        auto &lock = const_cast<lock::spinlock_t &>(lock_);
        uctx::RawSpinLockUninterruptibleContext guard(lock);
        return foreground_process_group_.load(std::memory_order_relaxed);
    }

    // Return a foreground snapshot only while the identity is live.  The
    // live check and snapshot share the same linearization point as revoke
    // and foreground updates; callers may signal the snapshot after unlock.
    bool foreground_if_live(group_id &foreground) const
    {
        auto &lock = const_cast<lock::spinlock_t &>(lock_);
        uctx::RawSpinLockUninterruptibleContext guard(lock);
        if (!live_.load(std::memory_order_relaxed))
            return false;
        foreground = foreground_process_group_.load(std::memory_order_relaxed);
        return true;
    }

  private:
    u64 id_;
    u64 generation_;
    u64 mode_;
    std::array<u8, 16> token_{};
    mutable lock::spinlock_t lock_;
    std::atomic_bool live_{true};
    std::atomic<::session_id> session_id_{0};
    std::atomic<group_id> foreground_process_group_{0};
    u64 io_lease_count_ = 0;
};
} // namespace dev::tty
