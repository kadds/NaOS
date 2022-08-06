#pragma once
#include "common.hpp"
#include "freelibcxx/assert.hpp"
namespace freelibcxx
{
void assert_fail(const char *expr, const char *file, int line, const char *msg);
}
