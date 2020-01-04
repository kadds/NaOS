#include "kernel/util/random.hpp"
namespace util
{

static u64 x = 123456789, y = 362436069, z = 521288629;
// period 2^96-1
u64 next_rand()
{
    u64 t;
    x ^= x << 16;
    x ^= x >> 5;
    x ^= x << 1;

    t = x;
    x = y;
    y = z;
    z = t ^ x ^ y;

    return z;
}

u64 next_rand(u64 max) { return next_rand() % max; }

} // namespace util
