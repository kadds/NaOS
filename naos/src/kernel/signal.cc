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

void sig_stop(process_t *proc, signal_info_t *info)
{
    trace::info("signal ", info->number, ": stop process ", proc->pid);
    proc->last_stop_signal = info->number;
    task::stop_process(proc);
}

void sig_continue(process_t *proc, signal_info_t *info)
{
    trace::info("signal ", info->number, ": continue process ", proc->pid);
    proc->wait_stop_reported.store(false);
    proc->last_stop_signal = 0;
    task::continue_process(proc);
}

#define IGRE sig_ignore,
#define KILL sig_kill,
#define DUMP sig_kill_dump,
#define STOP sig_stop,
#define CONT sig_continue,

signal_func_t default_signal_handler[max_signal_count] = {
    KILL KILL KILL KILL        // 0-3
        DUMP DUMP DUMP DUMP    // 4-7
        KILL KILL DUMP KILL    // 8-11
        KILL KILL KILL KILL    // 12-15
        KILL                    // 16 SIGSTKFLT
        IGRE CONT STOP STOP STOP STOP // 17-22
        IGRE IGRE IGRE IGRE IGRE IGRE IGRE IGRE // 23-30
};

void signal_pack_t::send(process_t *to, signal_num_t num, i64 error, i64 code, i64 status)
{
    if (to == nullptr || num == 0 || num >= max_signal_count)
        return;

    auto t = task::current();
    signal_info_t info(num, error, code, t == nullptr ? 0 : t->process->pid, t == nullptr ? 0 : t->tid, status);
    if (num == signal::sigkill || num == signal::sigstop || num == signal::sigcout)
    {
        default_signal_handler[num](to, &info);
        return;
    }

    if (unlikely(events.size() > 1024))
        return;
    if (!masks.is_valid(num))
    {
        default_signal_handler[num](to, &info);
    }
    else
    {
        if (!masks.is_ignore(num))
        {
            events.push_back(std::move(info));
            wait_queue.do_wake_up();
        }
    }
}

void signal_pack_t::wait(signal_info_t *info)
{
    while (1)
    {
        wait_queue.do_wait([this]() -> bool {
            signal_pack_t *that = this;
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
        });

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
