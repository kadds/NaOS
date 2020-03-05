#include "kernel/signal.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
namespace task
{

void sig_ignore(process_t *proc, signal_info_t *info) {}

void sig_kill(process_t *proc, signal_info_t *info)
{
    trace::info("signal ", info->number, ": kill process ", proc->pid);
    task::exit_process(proc, 0, 0);
}

void sig_kill_dump(process_t *proc, signal_info_t *info)
{
    trace::info("signal ", info->number, ": dump process ", proc->pid);
    task::exit_process(proc, 0, task::exit_control_flags::core_dump);
}

#define IGRE sig_ignore,
#define KILL sig_kill,
#define DUMP sig_kill_dump,
#define STOP sig_kill,
#define CONT sig_kill,

signal_func_t default_signal_handler[max_signal_count] = {
    KILL KILL KILL KILL DUMP DUMP DUMP DUMP KILL KILL DUMP KILL KILL KILL KILL KILL IGRE CONT STOP IGRE IGRE IGRE IGRE
        IGRE IGRE IGRE IGRE IGRE IGRE IGRE IGRE IGRE};

void signal_pack_t::send(process_t *to, signal_num_t num, i64 error, i64 code, i64 status)
{
    auto t = task::current();
    if (unlikely(events.size() > 1024))
        return;
    signal_info_t info(num, error, code, t->process->pid, t->tid, status);
    if (!masks.is_valid(num))
    {
        default_signal_handler[num](to, &info);
    }
    else
    {
        if (!masks.is_ignore(num))
        {
            events.push_back(info);
            wait_queue.do_wake_up();
        }
    }
}

void signal_pack_t::wait(signal_info_t *info)
{
    while (1)
    {
        wait_queue.do_wait(
            [](u64 data) -> bool {
                signal_pack_t *that = (signal_pack_t *)data;
                auto &ev = that->get_events();
                auto &mask = that->get_mask();
                for (auto &e : ev)
                {
                    if (!mask.is_block(e.number))
                    {
                        return true;
                    }
                }
                return false;
            },
            (u64)this);

        auto &mask = this->get_mask();
        auto &ev = this->get_events();
        for (auto e = ev.begin(); e != ev.end();)
        {
            if (!masks.is_valid(e->number))
            {
                default_signal_handler[e->number](current_process(), &(*e));
                e = ev.remove(e);
            }
            else if (mask.is_ignore(e->number))
            {
                e = ev.remove(e);
            }
            else if (!mask.is_block(e->number))
            {
                *info = *e;
                e = ev.remove(e);
                return;
            }
        }
    }
}

} // namespace task
