#pragma once
#include "common.hpp"
#include "kernel/trace.hpp"
#include "task.hpp"
namespace syscall
{
ExportC void *system_call_table[128];

} // namespace syscall
