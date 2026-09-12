#pragma once
#include "kernel/common.hpp"
#include "kernel/task.hpp"
namespace task::builtin::vfsd
{
void main(task::thread_start_info_t *info);
} // namespace task::builtin::vfsd

namespace task::builtin::ramdiskd
{
void main(task::thread_start_info_t *info);
} // namespace task::builtin::ramdiskd
