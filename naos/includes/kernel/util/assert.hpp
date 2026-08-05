#pragma once
#include "freelibcxx/assert.hpp"
#include "kernel/common.hpp"
namespace freelibcxx
{
void assert_fail(const char *expr, const char *file, int line, const char *msg);
}
