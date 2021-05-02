
#include "comm.hpp"
#include "kernel/mm/new.hpp"
#include <hash_fun.h>
#include <iostream>
#include <memory.h>

LibAllocator LibAllocatorV;

namespace util
{
void cassert_fail(const char *expr, const char *file, int line)
{
    std::cerr << expr << " assert fail at " << file << ":" << line << std::endl;
    exit(-1);
}
} // namespace util