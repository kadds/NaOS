#include "kernel/util/cxxlib.hpp"
ExportC void __cxa_pure_virtual()
{
    while (1)
        ;
}

void operator delete(void *p, u64 log) // or delete(void *, std::size_t)
{
}