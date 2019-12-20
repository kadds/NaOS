#include "kernel/task/builtin/input_task.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/wait.hpp"
namespace task::builtin::input
{
void main(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    while (1)
    {
        io::keyboard_request_t request;
        request.type = io::chain_number::keyboard;
        request.cmd_type = io::keyboard_request_t::command::get_key;
        if (!io::send_io_request(&request))
        {
            trace::warning("error when send io request");
        }
        if (request.result.poll_status == 0)
        {
            if (request.result.cmd_type.get.release)
            {
                trace::debug("key ", (void *)request.result.cmd_type.get.key, " release");
            }
            else
            {
                trace::debug("key ", (void *)request.result.cmd_type.get.key, " press");
            }
        }

        {
            // io::keyboard_request_t request;
            // request.type = io::chain_number::keyboard;
            // request.cmd_type = io::keyboard_request_t::command::set_led_cas;
            // request.cmd_param = 1;
            // if (!io::send_io_request(&request))
            // {
            //     trace::warning("error when send io request");
            // }
        }
    };
}
} // namespace task::builtin::input
