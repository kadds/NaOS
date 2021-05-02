#pragma once
#include "common.hpp"
namespace util
{
u32 djb_hash(const char *str);
u64 murmur_hash2_64(const void *key, u64 len, u64 seed);
void murmur_hash3_128(const void *key, const u64 len, const uint32_t seed, void *out);

} // namespace util
