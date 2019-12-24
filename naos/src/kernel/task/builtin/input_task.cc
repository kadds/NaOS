#include "kernel/task/builtin/input_task.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/util/bit_set.hpp"
#include "kernel/wait.hpp"
namespace task::builtin::input
{
util::bit_set_inplace<256> key_state;

void complation(io::request_t *req, bool intr, u64 user_data)
{
    wait_queue *q = (wait_queue *)user_data;
    task::do_wake_up(q);
}

void print_keyboard(io::keyboard_result_t &res, io::status_t &status)
{
    if (status.io_is_completion)
    {
        status.io_is_completion = false;
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
    }
}

void print_mouse(io::mouse_result_t &res, io::status_t &status)
{
    if (status.io_is_completion)
    {
        status.io_is_completion = false;
        trace::debug("mouse x:", res.get.movement_x, " y:", res.get.movement_y, " at ", res.get.timestamp);
        if (res.get.down_x)
        {
            trace::debug("left down");
        }
        if (res.get.down_y)
        {
            trace::debug("right down");
        }
        if (res.get.down_z)
        {
            trace::debug("mid down");
        }
        if (res.get.movement_z)
        {
            trace::debug("scroll ", res.get.movement_z);
        }
    }
}

io::keyboard_request_t request;
io::mouse_request_t mreq;

bool wait_condition(u64 user_data) { return request.status.io_is_completion || mreq.status.io_is_completion; }

void main(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    key_state.clean_all();
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
    request.status.io_is_completion = false;
    mreq.status.io_is_completion = false;

    while (1)
    {
        if (!io::send_io_request(&request))
        {
            trace::warning("error when send io request");
        }
        if (!io::send_io_request(&mreq))
        {
            trace::warning("error when send io request");
        }
        if (request.status.io_is_completion || mreq.status.io_is_completion)
        {
        }
        else
        {
            task::do_wait(&input_wait_queue, wait_condition, 0, task::wait_context_type::uninterruptible);
        }
        print_keyboard(request.result, request.status);
        print_mouse(mreq.result, mreq.status);
        trace::debug("next io request");
    };
}
} // namespace task::builtin::input
