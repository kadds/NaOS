#include "kernel/time.hpp"
#include "kernel/timer.hpp"

#include "catch2_compat.hpp"
#include <cstdint>
#include <limits>

namespace
{
void test_converts_valid_timespec()
{
    timeclock::microsecond_t result = 0;
    REQUIRE(timeclock::try_to_microseconds(timeclock::time(3, 456789), result));
    REQUIRE(result == 3000456);
}

void test_truncates_sub_microsecond_precision()
{
    timeclock::microsecond_t result = 0;
    REQUIRE(timeclock::try_to_microseconds(timeclock::time(0, 999), result));
    REQUIRE(result == 0);
}

void test_rejects_invalid_timespec()
{
    timeclock::microsecond_t result = 0;
    REQUIRE(!timeclock::try_to_microseconds(timeclock::time(-1, 0), result));
    REQUIRE(!timeclock::try_to_microseconds(timeclock::time(0, -1), result));
    REQUIRE(!timeclock::try_to_microseconds(timeclock::time(0, 1'000'000'000), result));
}

void test_rejects_microsecond_overflow()
{
    timeclock::microsecond_t result = 0;
    REQUIRE(!timeclock::try_to_microseconds(timeclock::time(std::numeric_limits<std::int64_t>::max(), 0), result));
}

void test_rejects_deadline_overflow()
{
    timeclock::microsecond_t result = 0;
    const auto max = std::numeric_limits<timeclock::microsecond_t>::max();
    REQUIRE(timeclock::try_add_microseconds(max - 2, 2, result));
    REQUIRE(result == max);
    REQUIRE(!timeclock::try_add_microseconds(max - 1, 2, result));
}

void test_clock_source_validation_is_bounded() { REQUIRE(timer::source_validation_samples < 1'000'000); }
} // namespace

TEST_CASE("wait deadline conversion", "[wait][deadline]")
{
    test_converts_valid_timespec();
    test_truncates_sub_microsecond_precision();
    test_rejects_invalid_timespec();
    test_rejects_microsecond_overflow();
    test_rejects_deadline_overflow();
    test_clock_source_validation_is_bounded();
}
