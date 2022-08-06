#include "kernel/util/assert.hpp"
#include "kernel/kernel.hpp"
#include "kernel/trace.hpp"
namespace freelibcxx
{
void assert_fail(const char *expr, const char *file, int line, const char *msg)
{
    trace::panic(expr, " assert fail at ", file, ":", line, " with ", msg);
}
} // namespace util
