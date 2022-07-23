#include "kernel/handle.hpp"
#include "kernel/trace.hpp"

raw_handle_t::~raw_handle_t()
{
    // could be leak
}
