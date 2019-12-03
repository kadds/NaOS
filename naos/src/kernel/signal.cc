#include "kernel/signal.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
namespace task
{

enum class signal_type
{

};

void sig_ignore(signal_num_t num, signal_info_t *info){};

void sig_kill(signal_num_t num, signal_info_t *info) {}

signal_actions_t::signal_actions_t() {}

signal_func_t default_signal_handler[max_signal_count] = {sig_ignore, sig_ignore, sig_ignore, sig_ignore, sig_ignore};

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
                arch::task::userland_code_context context;
                arch::task::make_signal_context(signal_stack, (void *)act.handler, &context);
                arch::task::set_signal_param(&context, 0, info.number);
                if (act.flags & sig_action_flag_t::info)
                {
                    arch::task::set_signal_param(&context, 1, arch::task::copy_signal_param(&context, &it, sizeof(it)));
                }

                arch::task::enter_signal_context(&context);
            }
        }
    }

    sig_pending = false;
}

void signal_return()
{
    auto thread = task::current();
    if (unlikely(thread == nullptr))
        return;
    thread->signal_pack.set_in_signal(false);
}

void do_signal()
{
    auto thread = task::current();
    if (unlikely(thread == nullptr))
        return;
    if (thread->signal_pack.is_set() && !thread->signal_pack.is_in_signal())
        thread->signal_pack.dispatch(thread->process->signal_actions);
}

} // namespace task
