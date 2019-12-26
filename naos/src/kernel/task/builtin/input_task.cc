#include "kernel/task/builtin/input_task.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/util/bit_set.hpp"
#include "kernel/wait.hpp"
namespace task::builtin::input
{
util::bit_set_inplace<256> key_state;

io::mouse_data current_mouse_data;

void complation(io::request_t *req, bool intr, u64 user_data)
{
    wait_queue *q = (wait_queue *)user_data;
    task::do_wake_up(q);
}

void print_keyboard(io::keyboard_result_t &res, io::status_t &status, io::request_t *req)
{
    if (status.io_is_completion)
    {
        if (res.get.release)
        {
            key_state.clean(res.get.key);
            trace::debug("key ", (void *)res.get.key, " release at ", res.get.timestamp);
        }
        else
        {
            key_state.set(res.get.key);
            trace::debug("key ", (void *)res.get.key, " press at ", res.get.timestamp);
        }
        io::finish_io_request(req);
    }
}

void print_mouse(io::mouse_result_t &res, io::status_t &status, io::request_t *req)
{
    if (status.io_is_completion)
    {
        trace::debug("mouse x:", res.get.movement_x, " y:", res.get.movement_y, " at ", res.get.timestamp);

        if (res.get.down_x ^ current_mouse_data.down_x)
        {
            trace::debug(res.get.down_x ? "left button down" : "left button up");
            current_mouse_data.down_x = res.get.down_x;
        }

        if (res.get.down_y ^ current_mouse_data.down_y)
        {
            trace::debug(res.get.down_y ? "right button down" : "right button up");
            current_mouse_data.down_y = res.get.down_y;
        }

        if (res.get.down_z ^ current_mouse_data.down_z)
        {
            trace::debug(res.get.down_z ? "mid button down" : "mid button up");
            current_mouse_data.down_z = res.get.down_z;
        }

        if (res.get.down_a ^ current_mouse_data.down_a)
        {
            trace::debug(res.get.down_a ? "button 4 down" : "button 4 up");
            current_mouse_data.down_a = res.get.down_a;
        }

        if (res.get.down_b ^ current_mouse_data.down_b)
        {
            trace::debug(res.get.down_b ? "button 5 down" : "button 5 up");
            current_mouse_data.down_b = res.get.down_b;
        }

        if (res.get.movement_z)
        {
            trace::debug("scroll ", res.get.movement_z);
            current_mouse_data.movement_z = res.get.movement_z;
        }

        io::finish_io_request(req);
    }
}

io::keyboard_request_t request;
io::mouse_request_t mreq;

bool wait_condition(u64 user_data) { return request.status.io_is_completion || mreq.status.io_is_completion; }

void main(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    key_state.clean_all();
    current_mouse_data.down_x = current_mouse_data.down_y = current_mouse_data.down_z = current_mouse_data.down_a =
        current_mouse_data.down_b = false;

    current_mouse_data.movement_x = 0;
    current_mouse_data.movement_y = 0;
    current_mouse_data.movement_z = 0;
    current_mouse_data.timestamp = 0;

    wait_queue input_wait_queue(memory::KernelCommonAllocatorV);

    request.type = io::chain_number::keyboard;
    request.cmd_type = io::keyboard_request_t::command::get_key;
    request.final_completion_func = complation;
    request.completion_user_data = (u64)&input_wait_queue;
    request.poll = false;

    mreq.type = io::chain_number::mouse;
    mreq.cmd_type = io::mouse_request_t::command::get;
    mreq.final_completion_func = complation;
    mreq.completion_user_data = (u64)&input_wait_queue;
    mreq.poll = false;
    request.status.io_is_completion = true;
    mreq.status.io_is_completion = true;

    while (1)
    {
        if (request.status.io_is_completion)
        {
            request.status.io_is_completion = false;
            if (!io::send_io_request(&request))
            {
                trace::warning("error when send io request");
            }
        }
        if (mreq.status.io_is_completion)
        {
            mreq.status.io_is_completion = false;
            if (!io::send_io_request(&mreq))
            {
                trace::warning("error when send io request");
            }
        }

        if (request.status.io_is_completion || mreq.status.io_is_completion)
        {
        }
        else
        {
            task::do_wait(&input_wait_queue, wait_condition, 0, task::wait_context_type::uninterruptible);
        }
        print_keyboard(request.result, request.status, &request);
        print_mouse(mreq.result, mreq.status, &mreq);
    };
}
} // namespace task::builtin::input
