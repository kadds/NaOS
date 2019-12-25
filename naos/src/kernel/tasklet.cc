#include "kernel/tasklet.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/ucontext.hpp"
namespace irq
{
tasklet_t *tasklet_head;
lock::spinlock_t lock;

void do_tasklet(u64 vec, u64 user_data) { exec_tasklet(); }

void init_tasklet()
{
    irq::insert_soft_request_func(soft_vector::task, do_tasklet, 0);
    tasklet_head = nullptr;
}

void add_tasklet(tasklet_t *tasklet)
{
    tasklet->state = 0;
    tasklet->next_cpu = nullptr;
    tasklet->enable = 0;
    tasklet->next = tasklet_head;
    tasklet_head = tasklet;
}

void raise_tasklet(tasklet_t *tasklet)
{
    {
        uctx::RawSpinLockUnInterruptableContext utx(lock);
        auto &cpu = cpu::current();
        tasklet->next_cpu = (tasklet_t *)cpu.get_tasklet_queue();
        cpu.set_tasklet_queue(tasklet);
    }
    raise_soft_irq(soft_vector::task);
}

void exec_tasklet()
{
    uctx::RawSpinLockUnInterruptableContextController utx(lock);
    auto &cpu = cpu::current();
    utx.begin();
    auto tasklet = (tasklet_t *)cpu.get_tasklet_queue();
    cpu.set_tasklet_queue(nullptr);
    utx.end();

    while (tasklet != nullptr)
    {
        auto next = tasklet->next_cpu;
        tasklet->next_cpu = nullptr;
        if (tasklet->enable >= 0)
        {
            tasklet->state = 1;
            tasklet->func(tasklet->user_data);
            tasklet->state = 0;
        }
        tasklet = next;
    }
}

} // namespace irq
