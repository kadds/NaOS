#include "kernel/task/builtin/input_task.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/wait.hpp"
namespace task::builtin::input
{
void main(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    io::keyboard_request_t request;
    request.type = io::chain_number::keyboard;
    request.cmd_type = io::keyboard_request_t::command::get_key;

    io::mouse_request_t mreq;
    mreq.type = io::chain_number::mouse;
    mreq.cmd_type = io::mouse_request_t::command::get;

    while (1)
    {
        if (!io::send_io_request(&request))
        {
            trace::warning("error when send io request");
            continue;
        }
        if (request.result.poll_status == 0)
        {
            if (request.result.cmd_type.get.release)
            {
                trace::debug("key ", (void *)request.result.cmd_type.get.key, " release at ",
                             request.result.cmd_type.get.timestamp);
            }
            else
            {
                trace::debug("key ", (void *)request.result.cmd_type.get.key, " press at ",
                             request.result.cmd_type.get.timestamp);
            }
        }

        if (!io::send_io_request(&mreq))
        {
            trace::warning("error when send io request");
            continue;
        }
        if (mreq.result.poll_status == 0)
        {
            trace::debug("mouse x:", mreq.result.cmd_type.get.movement_x, " y:", mreq.result.cmd_type.get.movement_y,
                         " at ", mreq.result.cmd_type.get.timestamp);
            if (mreq.result.cmd_type.get.down_x)
            {
                trace::debug("left down");
            }
            if (mreq.result.cmd_type.get.down_y)
            {
                trace::debug("right down");
            }
            if (mreq.result.cmd_type.get.down_z)
            {
                trace::debug("mid down");
            }
            if (mreq.result.cmd_type.get.movement_z)
            {
                trace::debug("scroll ", mreq.result.cmd_type.get.movement_z);
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
