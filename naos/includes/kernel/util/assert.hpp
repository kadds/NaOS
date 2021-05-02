#pragma once
#include "common.hpp"
namespace util
{
void cassert_fail(const char *expr, const char *file, int line);
}

#define cassert(expr)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (unlikely(!(expr)))                                                                                         \
        {                                                                                                              \
            ::util::cassert_fail(#expr, __FILE__, __LINE__);                                                           \
        }                                                                                                              \
    } while (0)
