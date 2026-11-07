#pragma once

#include "kernel/types.hpp"

namespace acpi
{
enum class transition_kind : u8
{
    sleep,
    reset,
};

enum class transition_prepare_result : u8
{
    prepared,
    conflict,
    exhausted,
};

/// Caller-serialized ACPI sleep/reset ticket lifecycle; it performs no I/O.
class transition_lifecycle
{
  public:
    static constexpr u64 ticket_lifetime_ns = 30'000'000'000ULL;

    transition_lifecycle() = default;
    explicit transition_lifecycle(u64 next_ticket)
        : next_ticket_(next_ticket)
    {
    }

    transition_prepare_result prepare(transition_kind kind, u32 sleep_type_a, u32 sleep_type_b, u64 now_ns,
                                      bool timeout_supported, u64 &ticket)
    {
        (void)expire(now_ns);
        if (phase_ != phase::none)
            return transition_prepare_result::conflict;
        if (next_ticket_ == 0)
            return transition_prepare_result::exhausted;

        ticket_ = next_ticket_;
        next_ticket_ = next_ticket_ == ~u64{0} ? 0 : next_ticket_ + 1;
        kind_ = kind;
        phase_ = phase::prepared;
        expires_ns_ = !timeout_supported                      ? 0
                      : now_ns > ~u64{0} - ticket_lifetime_ns ? ~u64{0}
                                                              : now_ns + ticket_lifetime_ns;
        sleep_type_a_ = kind == transition_kind::sleep ? sleep_type_a : 0;
        sleep_type_b_ = kind == transition_kind::sleep ? sleep_type_b : 0;
        ticket = ticket_;
        return transition_prepare_result::prepared;
    }

    bool cancel(u64 ticket, u64 now_ns)
    {
        (void)expire(now_ns);
        if (phase_ != phase::prepared || ticket == 0 || ticket_ != ticket)
            return false;
        clear();
        return true;
    }

    bool begin_commit(transition_kind kind, u64 ticket, u64 now_ns, u32 &sleep_type_a, u32 &sleep_type_b)
    {
        (void)expire(now_ns);
        if (phase_ != phase::prepared || kind_ != kind || ticket == 0 || ticket_ != ticket)
            return false;
        phase_ = phase::committing;
        sleep_type_a = sleep_type_a_;
        sleep_type_b = sleep_type_b_;
        return true;
    }

    void finish() { clear(); }

    void owner_exit() { clear(); }

    bool expire(u64 now_ns)
    {
        if (phase_ != phase::prepared || expires_ns_ == 0 || now_ns < expires_ns_)
            return false;
        clear();
        return true;
    }

    bool active(u64 now_ns)
    {
        (void)expire(now_ns);
        return phase_ != phase::none;
    }

    bool committing() const { return phase_ == phase::committing; }

  private:
    enum class phase : u8
    {
        none,
        prepared,
        committing,
    };

    void clear()
    {
        phase_ = phase::none;
        ticket_ = 0;
        expires_ns_ = 0;
        sleep_type_a_ = 0;
        sleep_type_b_ = 0;
    }

    phase phase_ = phase::none;
    transition_kind kind_ = transition_kind::sleep;
    u64 ticket_ = 0;
    u64 next_ticket_ = 1;
    u64 expires_ns_ = 0;
    u32 sleep_type_a_ = 0;
    u32 sleep_type_b_ = 0;
};
} // namespace acpi
