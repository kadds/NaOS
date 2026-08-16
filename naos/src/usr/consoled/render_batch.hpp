#pragma once

#include <cstdint>

namespace consoled
{
struct render_batch
{
    static constexpr std::uint64_t settle_delay_ms = 2;

    bool pending = false;
    std::uint64_t due_ms = 0;

    constexpr void request(std::uint64_t now_ms) noexcept
    {
        pending = true;
        due_ms = now_ms + settle_delay_ms;
    }

    constexpr bool ready(std::uint64_t now_ms) const noexcept { return pending && now_ms >= due_ms; }

    constexpr void consume() noexcept
    {
        pending = false;
        due_ms = 0;
    }
};
} // namespace consoled
