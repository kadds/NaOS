#include "kernel/signal.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
namespace task
{

void sig_ignore(signal_num_t num, signal_info_t *info) {}

void sig_kill(signal_num_t num, signal_info_t *info)
{
    trace::info("signal ", num, ": kill process ", task::current()->process->pid, " tid ", task::current()->tid);
    task::kill_thread(task::current(), task::thread_control_flags::process);
}

void sig_kill_dump(signal_num_t num, signal_info_t *info)
{
    trace::info("signal ", num, ": dump process ", task::current()->process->pid, " tid ", task::current()->tid);
    task::kill_thread(task::current(), task::thread_control_flags::process);
}

void sig_stop(signal_num_t num, signal_info_t *info)
{
    trace::info("signal ", num, ": stop process ", task::current()->process->pid, " tid ", task::current()->tid);
    task::stop_thread(task::current(), task::thread_control_flags::process);
}

void sig_cont(signal_num_t num, signal_info_t *info)
{
    trace::info("signal ", num, ": continue process ", task::current()->process->pid, " tid ", task::current()->tid);
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

void signal_actions_t::set_action(signal_num_t num, signal_func_t handler, signal_set_t set, flag_t flags)
{
    if (handler == nullptr)
    {
        handler = default_signal_handler[num];
    }
    else if (num == signal::sigsegv || num == signal::sigill || num == signal::sigkill)
    {
        if ((u64)handler < minimum_kernel_addr)
            return;
    }

    actions[num].handler = handler;
    actions[num].ignore_bitmap = set;
    actions[num].flags = flags;
}

void signal_actions_t::clean_all()
{
    for (u64 i = 0; i < max_signal_count; i++)
    {
        actions[i].handler = default_signal_handler[i];
        actions[i].flags = 0;
    }
}

signal_actions_t::signal_actions_t() { clean_all(); }

void signal_pack_t::set(signal_num_t num, i64 error, i64 code, i64 status)
{
    if (!masks.is_ignore(num))
    {
        sig_pending = true;
        thread_t *t = current();
        if (unlikely(events.size() > 1024))
            return;
        events.push_back(signal_info_t(num, error, code, t->process->pid, t->tid, status));
    }
}

void signal_pack_t::dispatch(signal_actions_t *actions)
{
    for (auto it = events.begin(); it != events.end(); ++it)
    {
        if (!masks.is_block(it->number))
        {
            auto info = *it;
            events.remove(it);
            auto &act = actions->get_action(info.number);
            current_signal_sent = info.number;
            if (is_kernel_space_pointer(act.handler))
            {
                act.handler(info.number, &it);
            }
            else
            {
                in_signal = true;
                if (!arch::task::make_signal_context(signal_stack, (void *)act.handler, &context))
                {
                    // kill process
                    trace::info("process ", current()->process->pid,
                                " can't make signal context because userland stack is too small.");
                    sig_kill_dump(signal::sigsegv, &it);
                }
                arch::task::set_signal_param(&context, 0, info.number);
                if (act.flags & sig_action_flag_t::info)
                {
                    arch::task::set_signal_param(&context, 1, arch::task::copy_signal_param(&context, &it, sizeof(it)));
                }
                arch::task::set_signal_context(&context);
                break;
            }
        }
    }

    sig_pending = events.size() != 0;
}

void signal_pack_t::user_return(u64 code)
{
    in_signal = false;
    arch::task::return_from_signal_context(&context, code);
}

void signal_return(u64 code)
{
    auto thread = task::current();
    if (unlikely(thread == nullptr))
        return;
    if (thread->signal_pack.is_in_signal())
        thread->signal_pack.user_return(code);
}

void do_signal()
{
    auto thread = task::current();
    if (unlikely(thread == nullptr))
        return;
    if (thread->signal_pack.is_set())
        thread->signal_pack.dispatch(thread->process->signal_actions);
}

} // namespace task
