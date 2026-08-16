#include "kernel/task/builtin/input_task.hpp"
#include "freelibcxx/bit_set.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/common/cursor/cursor.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/handle.hpp"
#include "kernel/input_event_source.hpp"
#include "kernel/input/key.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/signal.hpp"
#include "kernel/terminal.hpp"
#include "kernel/timer.hpp"
#include "kernel/wait.hpp"

namespace task::builtin::input
{
freelibcxx::bit_set_inplace<256> key_down_state;

io::mouse_data current_mouse_data;

using ::input::key;

bool is_key_down(key k) { return key_down_state.get_bit((u64)k); }

bool is_ctrl_key_down() { return is_key_down(key::left_control) || is_key_down(key::right_control); }
bool is_alt_key_down() { return is_key_down(key::left_alt) || is_key_down(key::right_alt); }

constexpr u64 event_mod_ctrl = 1;
constexpr u64 event_mod_alt = 2;
constexpr u64 event_mod_shift = 4;
constexpr u64 event_kind_press = NA_INPUT_EVENT_KIND_PRESS;
constexpr u64 event_kind_release = NA_INPUT_EVENT_KIND_RELEASE;

void publish_key_event(key k, bool pressed)
{
    auto *source = dev::input::get_input_event_source();
    if (source == nullptr)
        return;
    u64 modifiers = 0;
    if (is_ctrl_key_down())
        modifiers |= event_mod_ctrl;
    if (is_alt_key_down())
        modifiers |= event_mod_alt;
    if (is_key_down(key::left_shift) || is_key_down(key::right_shift))
        modifiers |= event_mod_shift;
    source->publish(static_cast<u64>(k), modifiers, pressed ? event_kind_press : event_kind_release);
}

void print_keyboard(io::keyboard_result_t &res, io::status_t &status, io::request_t *req)
{
    if (status.io_is_completion)
    {
        if (res.get.release)
        {
            key_down_state.reset_bit(res.get.key);
            publish_key_event((key)res.get.key, false);
        }
        else
        {
            key k = (key)res.get.key;
            if (is_ctrl_key_down() && is_alt_key_down() && k == key::f12)
            {
                auto terms = term::get_terms();
                const auto to_idx = term::terminal_manager::kernel_console_index;
                if (terms->valid_index(to_idx))
                {
                    terms->switch_term(to_idx);
                }
            }
            else
                publish_key_event(k, true);
            key_down_state.set_bit(res.get.key);
        }
        io::finish_io_request(req);
    }
}

timeclock::microsecond_t last_update_mouse_time;
void print_mouse(io::mouse_result_t &res, const io::status_t &status, io::request_t *req, handle_t<fs::vfs::file> f)
{
    if (status.io_is_completion)
    {
        // trace::debug("mouse x:", res.get.movement_x, " y:", res.get.movement_y, " at ", res.get.timestamp);
        auto current_cursor = cursor::get_cursor();
        current_cursor.x += res.get.movement_x;
        current_cursor.y -= res.get.movement_y;
        current_cursor.state = cursor::state_t::normal;

        cursor::set_cursor(current_cursor);
        last_update_mouse_time = timer::get_high_resolution_time();

        if (res.get.down_x ^ current_mouse_data.down_x)
        {
            // trace::debug(res.get.down_x ? "left button down" : "left button up");
            current_mouse_data.down_x = res.get.down_x;
        }

        if (res.get.down_y ^ current_mouse_data.down_y)
        {
            // trace::debug(res.get.down_y ? "right button down" : "right button up");
            current_mouse_data.down_y = res.get.down_y;
        }

        if (res.get.down_z ^ current_mouse_data.down_z)
        {
            // trace::debug(res.get.down_z ? "mid button down" : "mid button up");
            current_mouse_data.down_z = res.get.down_z;
        }

        if (res.get.down_a ^ current_mouse_data.down_a)
        {
            // trace::debug(res.get.down_a ? "button 4 down" : "button 4 up");
            current_mouse_data.down_a = res.get.down_a;
        }

        if (res.get.down_b ^ current_mouse_data.down_b)
        {
            // trace::debug(res.get.down_b ? "button 5 down" : "button 5 up");
            current_mouse_data.down_b = res.get.down_b;
        }

        if (res.get.movement_z)
        {
            // trace::debug("scroll ", res.get.movement_z);
            current_mouse_data.movement_z = res.get.movement_z;
        }

        io::finish_io_request(req);
    }
}

io::keyboard_request_t request;
void listen_keyboard()
{
    key_down_state.reset_all();
    request.type = io::chain_number::keyboard;
    request.cmd_type = io::keyboard_request_t::command::get_key;
    request.status.io_is_completion = true;
    wait_queue_t input_wait_queue;
    auto completion = [&input_wait_queue](io::request_t *, bool) noexcept { input_wait_queue.do_wake_up(); };
    request.final_completion_func = io::completion_result_func_t::borrow(completion);
    request.poll = false;
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
        if (!request.status.io_is_completion)
        {
            input_wait_queue.do_wait([] { return request.status.io_is_completion.load(); });
        }
        print_keyboard(request.result, request.status, &request);
    };
}

io::mouse_request_t mreq;
void listen_mouse()
{
    fs::vfs::create("/dev/mouse_input", fs::vfs::global_root, fs::vfs::global_root, fs::create_flags::chr);

    auto mouse_file = fs::vfs::open("/dev/mouse_input", fs::vfs::global_root, fs::vfs::global_root, fs::mode::write, 0);

    current_mouse_data.down_x = current_mouse_data.down_y = current_mouse_data.down_z = current_mouse_data.down_a =
        current_mouse_data.down_b = false;

    current_mouse_data.movement_x = 0;
    current_mouse_data.movement_y = 0;
    current_mouse_data.movement_z = 0;
    current_mouse_data.timestamp = 0;
    last_update_mouse_time = timer::get_high_resolution_time();

    mreq.type = io::chain_number::mouse;
    mreq.cmd_type = io::mouse_request_t::command::get;
    wait_queue_t input_wait_queue;
    auto completion = [&input_wait_queue](io::request_t *, bool) noexcept { input_wait_queue.do_wake_up(); };
    mreq.final_completion_func = io::completion_result_func_t::borrow(completion);
    mreq.poll = false;
    mreq.status.io_is_completion = true;

    while (1)
    {
        if (mreq.status.io_is_completion)
        {
            mreq.status.io_is_completion = false;
            if (!io::send_io_request(&mreq))
            {
                trace::warning("error when send io request");
            }
        }

        if (!mreq.status.io_is_completion)
        {
            input_wait_queue.do_wait([] { return mreq.status.io_is_completion.load(); });
        }
        print_mouse(mreq.result, mreq.status, &mreq, mouse_file);
    };
}

void main(task::thread_start_info_t *info)
{
    task::create_thread(task::current_process(), (task::thread_start_func)listen_mouse, nullptr, 0,
                        create_thread_flags::real_time_rr);
    listen_keyboard();
}
} // namespace task::builtin::input
