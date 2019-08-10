#include "kernel/trace.hpp"
#include "kernel/ScreenPrinter.hpp"
#include <stdarg.h>
namespace trace
{

NoReturn void panic(const char *msg)
{
    gPrinter->printf("panic: ");
    gPrinter->printf(msg);
    gPrinter->printf("\n");
    for (;;)
        ;
}
void assert_runtime(const char *exp, const char *file, int line, ...)
{
    gPrinter->printf("runtime assert failed: at file: %s:%d\n", file, line);
    va_list ap;
    va_start(ap, 0);
    const char *last = va_arg(ap, const char *);
    while (last != 0)
    {
        gPrinter->printf(last);
        last = va_arg(ap, const char *);
    }
    gPrinter->printf("\n  ");
    va_end(ap);
    panic("");
}
} // namespace trace
