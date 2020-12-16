#include "kernel/irq.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/interrupt.hpp"
#include "kernel/cpu.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/tasklet.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/util/linked_list.hpp"
#include "kernel/wait.hpp"
namespace irq
{
using request_list_t = util::linked_list<request_func_data>;
using request_list_node_allocator_t = memory::list_node_cache_allocator<request_list_t>;

struct request_lock_list_t
{
    lock::rw_lock_t lock;
    request_list_t *list;
    explicit request_lock_list_t(memory::IAllocator *a)
        : list(memory::New<request_list_t>(memory::KernelCommonAllocatorV, a))
    {
    }
};

request_lock_list_t *irq_list;
request_lock_list_t *soft_irq_list;

request_list_node_allocator_t *list_allocator;

const int irq_count = 256;

bool _ctx_interrupt_ do_irq(const regs_t *regs, u64 extra_data)
{
    auto &locked_list = irq_list[regs->vector];
    if (!locked_list.list->empty())
    {
        uctx::RawReadLockContext icu(locked_list.lock);
        bool ok = false;
        for (auto &it : *locked_list.list)
        {
            auto ret = it.hard_func(regs, extra_data, it.user_data);
            if (ret == request_result::ok)
                ok = true;
        }
        return ok;
    }
    return false;
}

bool wakeup_condition(u64 ud);

void do_soft_irq()
{
    auto &cpu = arch::cpu::current();
    for (int i = 0; i < soft_vector::COUNT; i++)
    {
        if (cpu.is_irq_pending(i))
        {
            uctx::RawReadLockUninterruptibleController ctr(soft_irq_list[i].lock);
            ctr.begin();
            if (cpu.is_irq_pending(i))
            {
                cpu.clean_irq_pending(i);
                request_list_t &list = *soft_irq_list[i].list;
                for (auto &it : list)
                {
                    request_func_data fd = it;
                    ctr.end();
                    fd.soft_func(i, fd.user_data);
                    ctr.begin();
                }
            }
            ctr.end();
        }
    }
    if (unlikely(wakeup_condition(0)))
        cpu::current().get_soft_irq_wait_queue()->do_wake_up();
}

bool check_and_wakeup_soft_irq(const regs_t *regs, u64 extra_data)
{
    do_soft_irq();
    return true;
}

bool wakeup_condition(u64 user_data)
{
    auto &cpu = arch::cpu::current();
    for (int i = 0; i < soft_vector::COUNT; i++)
    {
        if (cpu.is_irq_pending(i))
            return true;
    }
    return false;
}

void wakeup_soft_irq_daemon()
{
    cpu::current().get_soft_irq_wait_queue()->do_wait(wakeup_condition, 0);
    do_soft_irq();
}

void raise_soft_irq(u64 soft_irq_number)
{
    uctx::UninterruptibleContext uic;
    arch::cpu::current().set_irq_pending(soft_irq_number);
}

void init()
{
    if (cpu::current().is_bsp())
    {
        list_allocator = memory::New<request_list_node_allocator_t>(memory::KernelCommonAllocatorV);
        irq_list = memory::NewArray<request_lock_list_t>(memory::KernelCommonAllocatorV, irq_count, list_allocator);
        soft_irq_list =
            memory::NewArray<request_lock_list_t>(memory::KernelCommonAllocatorV, soft_vector::COUNT, list_allocator);
    }
    arch::exception::set_callback(&do_irq);
    arch::interrupt::set_callback(&do_irq);
    arch::interrupt::set_soft_irq_callback(&check_and_wakeup_soft_irq);
    init_tasklet();
    arch::idt::enable();
}

void insert_request_func(u32 vector, request_func func, u64 user_data)
{
    auto &locked_list = irq_list[vector];
    uctx::RawWriteLockUninterruptibleContext icu(locked_list.lock);
    locked_list.list->push_back(request_func_data((void *)func, user_data));
}

void remove_request_func(u32 vector, request_func func, u64 user_data)
{
    auto &locked_list = irq_list[vector];
    uctx::RawWriteLockUninterruptibleContext icu(locked_list.lock);
    auto &list = *locked_list.list;
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (it->hard_func == func && it->user_data == user_data)
        {
            list.remove(it);
            return;
        }
    }
}

void insert_soft_request_func(u32 vector, soft_request_func func, u64 user_data)
{
    auto &locked_list = soft_irq_list[vector];
    uctx::RawWriteLockUninterruptibleContext icu(locked_list.lock);
    locked_list.list->push_back(request_func_data((void *)func, user_data));
}

void remove_soft_request_func(u32 vector, soft_request_func func, u64 user_data)
{
    auto &locked_list = soft_irq_list[vector];
    uctx::RawWriteLockUninterruptibleContext icu(locked_list.lock);
    request_list_t &list = *locked_list.list;
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (it->soft_func == func && it->user_data == user_data)
        {
            list.remove(it);
            return;
        }
    }
}

} // namespace irq