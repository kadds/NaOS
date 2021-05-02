#include "kernel/cmdline.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/hash_map.hpp"
#include "kernel/util/memory.hpp"
#include "kernel/util/str.hpp"

namespace cmdline
{
using namespace util;
util::hash_map<string, string> *cmdmap;

void parse(const char *cmdline)
{
    cmdmap =
        memory::New<util::hash_map<string, string>>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    trace::debug("cmdline address ", reinterpret_cast<coaddr_t>(cmdline), ". ", cmdline);

    string ss(cmdline);

    for (auto item : *cmdmap)
    {
        trace::debug("args ", item.key.data(), "=", item.value.data());
    }
}
} // namespace cmdline