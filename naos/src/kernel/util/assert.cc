#include "kernel/util/assert.hpp"
#include "kernel/kernel.hpp"
#include "kernel/trace.hpp"
namespace freelibcxx
{
void assert_fail(const char *expr, const char *file, int line, const char *msg)
{
    trace::panic(expr, " assert fail at ", file, ":", line, " with ", msg);
}
} // namespace freelibcxx

namespace std
{
[[noreturn]] void __glibcxx_assert_fail(const char *file, int line, const char *function,
                                        const char *condition) noexcept
{
    freelibcxx::assert_fail(condition, file, line, function);

    // The kernel implementation panics, but keep this function noreturn even
    // if a different freelibcxx assertion handler is linked in.
    while (true)
    {
    }
}
} // namespace std
