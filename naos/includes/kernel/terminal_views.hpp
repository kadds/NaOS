#pragma once

#include "freelibcxx/vector.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/terminal_identity.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>

namespace dev::tty
{
class terminal_job_control final : public kobject
{
  public:
    static constexpr kobject::type_e type_of() { return kobject::type_e::terminal_job_control; }

    explicit terminal_job_control(handle_t<terminal_identity> identity)
        : kobject(kobject::type_e::terminal_job_control)
        , identity_(std::move(identity))
    {
    }

    bool capability_is_unique() const override { return false; }
    terminal_identity *identity() { return identity_.operator&(); }
    handle_t<terminal_identity> identity_handle() const { return identity_; }

  private:
    handle_t<terminal_identity> identity_;
};

class terminal_driver_control final : public kobject
{
  public:
    static constexpr kobject::type_e type_of() { return kobject::type_e::terminal_driver_control; }

    explicit terminal_driver_control(handle_t<terminal_identity> identity)
        : kobject(kobject::type_e::terminal_driver_control)
        , identity_(std::move(identity))
    {
    }

    bool capability_is_unique() const override { return true; }
    terminal_identity *identity() { return identity_.operator&(); }
    handle_t<terminal_identity> identity_handle() const { return identity_; }
    void on_capability_acquire(capability::location where) override;
    void on_capability_release(capability::location where) override;
    void on_capability_handoff(capability::location from, capability::location to) override;

  private:
    void release_capability();

    handle_t<terminal_identity> identity_;
    std::atomic<u64> capability_references_{0};
};

class terminal_driver_factory final : public kobject
{
  public:
    static constexpr kobject::type_e type_of() { return kobject::type_e::terminal_driver_factory; }

    terminal_driver_factory()
        : kobject(kobject::type_e::terminal_driver_factory)
        , identities_(memory::KernelCommonAllocatorV)
    {
        identities_.ensure(max_identities);
    }

    bool capability_is_unique() const override { return false; }

    bool create(u64 mode, khandle &identity_out, khandle &job_control_out, khandle &driver_control_out)
    {
        {
            constexpr u64 known_mode = 1 | 2 | 4;
            if ((mode & ~known_mode) != 0)
                return false;
        }

        u64 id = 0;
        constexpr u64 generation = 1;
        std::array<u8, 16> token{};
        u64 first = 0;
        u64 second = 0;
        // PTY locators are bearer capabilities. Refuse creation when the
        // platform cannot provide cryptographic hardware entropy instead of
        // constructing guessable tokens from clocks or counters.
        if (!entropy_word(first) || !entropy_word(second))
            return false;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            for (u64 i = 0; i < identities_.size();)
            {
                auto *existing = identities_[i].as<terminal_identity>().operator&();
                if (existing == nullptr || !existing->live())
                {
                    identities_.remove_at(i);
                    continue;
                }
                i++;
            }
            if (identities_.size() >= max_identities || identities_.capacity() < max_identities)
                return false;
            id = next_id_++;
            for (u64 i = 0; i < 8; i++)
            {
                token[i] = static_cast<u8>(first >> (i * 8));
                token[i + 8] = static_cast<u8>(second >> (i * 8));
            }
        }

        auto identity = handle_t<terminal_identity>::make(id, generation, mode, token);
        auto job_control = handle_t<terminal_job_control>::make(identity);
        auto driver_control = handle_t<terminal_driver_control>::make(identity);
        if (!identity || !job_control || !driver_control)
            return false;

        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            if (identities_.size() >= max_identities)
                return false;
            identities_.push_back(identity);
        }
        identity_out = identity;
        job_control_out = job_control;
        driver_control_out = driver_control;
        return true;
    }

    bool validate_locator(u64 id, u64 generation, const u8 *token, u64 token_size)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto now = timer::get_high_resolution_time();
        if (validation_window_start_ == 0 || now - validation_window_start_ >= validation_window_us)
        {
            validation_window_start_ = now;
            validation_failures_ = 0;
        }
        if (validation_failures_ >= validation_failure_limit)
            return false;
        for (auto &value : identities_)
        {
            auto *identity = value.as<terminal_identity>().operator&();
            if (identity != nullptr && identity->matches_locator(id, generation, token, token_size))
                return true;
        }
        validation_failures_++;
        return false;
    }

    void rollback(u64 id, u64 generation)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 i = 0; i < identities_.size(); i++)
        {
            auto *identity = identities_[i].as<terminal_identity>().operator&();
            if (identity == nullptr || identity->id() != id || identity->generation() != generation)
                continue;
            (void)identity->revoke();
            identities_.remove_at(i);
            return;
        }
    }

  private:
    static constexpr u64 max_identities = 256;
    static constexpr u64 validation_window_us = 1'000'000;
    static constexpr u64 validation_failure_limit = 64;

    static bool entropy_word(u64 &value)
    {
        u32 eax = 7;
        u32 ebx = 0;
        u32 ecx = 0;
        u32 edx = 0;
        _cpu_id(&eax, &ebx, &ecx, &edx);
        if ((ebx & (1U << 18)) != 0)
        {
            value = 0;
            unsigned char valid = 0;
            for (u32 attempt = 0; attempt < 8; attempt++)
            {
                __asm__ __volatile__("rdseed %0; setc %1" : "=r"(value), "=qm"(valid) : : "cc");
                if (valid != 0)
                    return true;
            }
        }

        eax = 1;
        ebx = 0;
        ecx = 0;
        edx = 0;
        _cpu_id(&eax, &ebx, &ecx, &edx);
        if ((ecx & (1U << 30)) != 0)
        {
            value = 0;
            unsigned char valid = 0;
            for (u32 attempt = 0; attempt < 8; attempt++)
            {
                __asm__ __volatile__("rdrand %0; setc %1" : "=r"(value), "=qm"(valid) : : "cc");
                if (valid != 0)
                    return true;
            }
        }

        return false;
    }

    lock::spinlock_t lock_;
    u64 next_id_ = 1;
    u64 validation_window_start_ = 0;
    u64 validation_failures_ = 0;
    freelibcxx::vector<khandle> identities_;
};
} // namespace dev::tty
