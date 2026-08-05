#pragma once
#include "kernel/common.hpp"
namespace task
{

void yield_preempt();

void disable_preempt();

void enable_preempt();
} // namespace task
