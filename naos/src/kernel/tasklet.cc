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
    irq::register_soft_request_func(soft_vector::task, do_tasklet, 0);
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
    bool queue_tasklet = false;
    {
        uctx::RawSpinLockUninterruptibleContext utx(lock);
        if (tasklet->state == 0)
        {
            auto &cpu = cpu::current();
            tasklet->state = 1;
            tasklet->next_cpu = (tasklet_t *)cpu.get_tasklet_queue();
            cpu.set_tasklet_queue(tasklet);
            queue_tasklet = true;
        }
        else
        {
            // A tasklet can receive another interrupt while it is still in
            // the per-CPU queue or executing. Do not link it twice: keep a
            // single queued node and request one more pass after this pass.
            tasklet->state = 2;
        }
    }
    if (queue_tasklet)
        raise_soft_irq(soft_vector::task);
}

void exec_tasklet()
{
    uctx::RawSpinLockUninterruptibleController utx(lock);
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
            tasklet->func(tasklet->user_data);
        }

        bool rerun = false;
        {
            uctx::RawSpinLockUninterruptibleContext ctx(lock);
            if (tasklet->state == 2 && tasklet->enable >= 0)
            {
                auto &current_cpu = cpu::current();
                tasklet->state = 1;
                tasklet->next_cpu = (tasklet_t *)current_cpu.get_tasklet_queue();
                current_cpu.set_tasklet_queue(tasklet);
                rerun = true;
            }
            else
            {
                tasklet->state = 0;
            }
        }
        if (rerun)
            raise_soft_irq(soft_vector::task);
        tasklet = next;
    }
}

} // namespace irq
