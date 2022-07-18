#pragma once
#include "common.hpp"
#include "kernel/task.hpp"
namespace task::builtin::softirq
{
void main(task::thread_start_info_t *info);
} // namespace task::builtin::softirq
