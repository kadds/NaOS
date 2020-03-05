#include "kernel/signal.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
namespace task
{

void sig_ignore(signal_num_t num, signal_info_t *info) {}

void sig_kill(signal_num_t num, signal_info_t *info)
{
    trace::info("signal ", num, ": kill process ", task::current()->process->pid);
    task::kill_thread(task::current(), task::thread_control_flags::process);
}

void sig_kill_dump(signal_num_t num, signal_info_t *info)
{
    trace::info("signal ", num, ": dump process ", task::current()->process->pid);
    task::kill_thread(task::current(), task::thread_control_flags::process | task::thread_control_flags::core_dump);
}

void sig_stop(signal_num_t num, signal_info_t *info)
{
    trace::info("signal ", num, ": stop process ", task::current()->process->pid);
    task::stop_thread(task::current(), task::thread_control_flags::process);
}

void sig_cont(signal_num_t num, signal_info_t *info)
{
    trace::info("signal ", num, ": continue process ", task::current()->process->pid);
    task::continue_thread(task::current(), task::thread_control_flags::process);
}

#define IGRE sig_ignore,
#define KILL sig_kill,
#define DUMP sig_kill_dump,
#define STOP sig_stop,
#define CONT sig_cont,

signal_func_t default_signal_handler[max_signal_count] = {
    KILL KILL DUMP KILL DUMP DUMP DUMP DUMP KILL KILL DUMP KILL KILL KILL KILL KILL IGRE CONT STOP IGRE IGRE IGRE IGRE
        IGRE IGRE IGRE IGRE IGRE IGRE IGRE IGRE IGRE};

void signal_pack_t::set(signal_num_t num, i64 error, i64 code, i64 status)
{
    thread_t *t = current();
    if (unlikely(events.size() > 1024))
        return;
    signal_info_t info(num, error, code, t->process->pid, t->tid, status);
    if (!masks.is_valid(num))
    {
        default_signal_handler[num](num, &info);
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
            default_signal_handler[e->number](e->number, &(*e));
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

} // namespace task
