#pragma once
#include "freelibcxx/delegate.hpp"
#include "kernel/common.hpp"
namespace irq
{

using tasklet_func = freelibcxx::delegate<void() noexcept>;

struct tasklet_t
{
    tasklet_t *next;
    /// used by per cpu
    tasklet_t *next_cpu;
    tasklet_func func;
    /// 0: idle, 1: queued or running, 2: raised while queued or running
    u8 state;
    /// >= 0 enable, < 0 disable
    i8 enable;
};

void init_tasklet();
void add_tasklet(tasklet_t *tasklet);
void raise_tasklet(tasklet_t *tasklet);
void exec_tasklet();

} // namespace irq
