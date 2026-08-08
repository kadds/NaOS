#include "kernel/time.hpp"

#include <cassert>
#include <cstdint>
#include <limits>

namespace
{
void test_converts_valid_timespec()
{
    timeclock::microsecond_t result = 0;
    assert(timeclock::try_to_microseconds(timeclock::time(3, 456789), result));
    assert(result == 3456789);
}

void test_truncates_sub_microsecond_precision()
{
    timeclock::microsecond_t result = 0;
    assert(timeclock::try_to_microseconds(timeclock::time(0, 999), result));
    assert(result == 0);
}

void test_rejects_invalid_timespec()
{
    timeclock::microsecond_t result = 0;
    assert(!timeclock::try_to_microseconds(timeclock::time(-1, 0), result));
    assert(!timeclock::try_to_microseconds(timeclock::time(0, -1), result));
    assert(!timeclock::try_to_microseconds(timeclock::time(0, 1'000'000'000), result));
}

void test_rejects_microsecond_overflow()
{
    timeclock::microsecond_t result = 0;
    assert(!timeclock::try_to_microseconds(
        timeclock::time(std::numeric_limits<std::int64_t>::max(), 0), result));
}
} // namespace

int main()
{
    test_converts_valid_timespec();
    test_truncates_sub_microsecond_precision();
    test_rejects_invalid_timespec();
    test_rejects_microsecond_overflow();
    return 0;
}
