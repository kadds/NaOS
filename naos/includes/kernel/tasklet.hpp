#pragma once
#include "common.hpp"
namespace irq
{

typedef void (*tasklet_func)(u64 user_data);

struct tasklet_t
{
    tasklet_t *next;
    /// used by per cpu
    tasklet_t *next_cpu;
    tasklet_func func;
    u64 user_data;
    /// 1: wait for runing
    u8 state;
    /// >= 0 enable, < 0 disable
    i8 enable;
};

void init_tasklet();
void add_tasklet(tasklet_t *tasklet);
void raise_tasklet(tasklet_t *tasklet);
void exec_tasklet();

} // namespace irq
