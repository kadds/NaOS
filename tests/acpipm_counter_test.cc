#include <catch2/catch_test_macros.hpp>

#include "kernel/time/wrapping_counter.hpp"

TEST_CASE("ACPI PM 24-bit counter extension stays monotonic across multiple wraps", "[acpipm_counter]")
{
    timeclock::wrapping_counter counter;
    constexpr u32 mask = (1u << 24) - 1;
    counter.reset(0, mask);

    for (u64 ticks = 4'000'000; ticks <= 48'000'000; ticks += 4'000'000)
        CHECK(counter.sample(static_cast<u32>(ticks) & mask) == ticks);
}

TEST_CASE("ACPI PM 32-bit counter extension crosses the unsigned wrap", "[acpipm_counter]")
{
    timeclock::wrapping_counter counter;
    constexpr u32 mask = ~u32(0);
    counter.reset(0xffff'fff0u, mask);

    CHECK(counter.sample(0xffff'fff8u) == 8);
    CHECK(counter.sample(0x0000'0010u) == 32);
}

TEST_CASE("ACPI PM counter clamps a stale concurrent sample instead of moving backwards", "[acpipm_counter]")
{
    timeclock::wrapping_counter counter;
    constexpr u32 mask = (1u << 24) - 1;
    counter.reset(0, mask);

    CHECK(counter.sample(1000) == 1000);
    CHECK(counter.sample(990) == 1000);
    CHECK(counter.sample(1010) == 1010);
}
