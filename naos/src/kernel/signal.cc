#include "kernel/signal.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/task.hpp"
namespace task
{

enum class signal_type
{

};

void sig_ignore(signal_num_t num){

};

void sig_kill(signal_num_t num) {}

void handle_signal(signal_num_t num, signal_func_t handler)
{
    if (is_kernel_space_pointer(handler))
        handler(num);
    else
    {
        /// TODO: do userland signal
    }
}

signal_actions_t::signal_actions_t() {}

signal_func_t default_signal_handler[max_signal_count] = {sig_ignore, sig_ignore, sig_ignore, sig_ignore, sig_ignore};

void signal_pack_t::set(signal_num_t num)
{
    if (!masks.is_ignore(num))
    {
        pending.set(num);
    }
}

void signal_pack_t::dispatch(signal_actions_t *actions)
{
    int num = pending.scan_set();
    while (num != -1)
    {
        if (!masks.is_block(num))
        {
            handle_signal(num, actions->get_action(num));
        }
        num = pending.scan_set();
    }
}

ExportC void do_signal()
{
    auto thread = task::current();
    if (unlikely(thread == nullptr))
        return;
    thread->signal_pack.dispatch(thread->process->signal_actions);
}

} // namespace task
