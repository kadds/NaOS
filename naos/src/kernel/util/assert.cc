#include "kernel/util/assert.hpp"
#include "kernel/kernel.hpp"
#include "kernel/log.hpp"
KLOG_MODULE(kernel);
namespace freelibcxx
{
void assert_fail(const char *expr, const char *file, int line, const char *msg)
{
    KLOG_PANIC("{} assert fail at {}:{} with {}", expr, file, line, msg);
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
