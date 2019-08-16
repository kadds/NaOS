#include "kernel/util/cxxlib.hpp"
#include "kernel/util/memory.hpp"

ExportC void __cxa_pure_virtual()
{
    while (1)
        ;
}

void operator delete(void *p, u64 log) // or delete(void *, std::size_t)
{
}

void operator delete(void *p) {}

ExportC void memset(void *dst, u64 val, u64 size) { util::memset(dst, val, size); }
ExportC void memcpy(void *dst, void *src, u64 size) { util::memcopy(dst, src, size); }
ExportC void memzero(void *dst, u64 size) { util::memzero(dst, size); }