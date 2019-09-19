#pragma once
#include "common.hpp"
namespace util
{
void memset(void *dst, u64 val, u64 size);
void memzero(void *dst, u64 size);
void memcopy(void *dst, const void *src, u64 size);
} // namespace util
