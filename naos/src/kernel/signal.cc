#include "kernel/signal.hpp"

namespace task
{

enum class signal_type
{

};

void sig_ignore(signal_num_t num){

};

void sig_kill(signal_num_t num) {}

void handle_userland_signal(signal_num_t num, signal_func_t handler) {}
void handle_kernel_signal(signal_num_t num, signal_func_t handler) { handler(num); }

signal_actions_t::signal_actions_t() {}

void signal_actions_t::make(signal_num_t num, thread_t *from, u64 user_data)
{
    actions[num].list.push_back(signal_pack(from, user_data));
}

signal_func_t default_signal_handler[max_signal_count] = {sig_ignore, sig_ignore, sig_ignore, sig_ignore, sig_ignore};

ExportC void do_signal() {}

} // namespace task
