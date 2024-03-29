#include "kernel/task/builtin/input_task.hpp"
#include "freelibcxx/bit_set.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/common/cursor/cursor.hpp"
#include "kernel/dev/tty/tty.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/handle.hpp"
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

freelibcxx::bit_set_inplace<32> key_switch_state;

char key_char_table[256] = {
    0,   0,   '1', '2', '3', '4', '5',  '6', '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e',  'r', 't',  'y',
    'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's', 'd', 'f', 'g', 'h', 'j',  'k',  'l', ';', '\'', '`', 0,    '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm',  ',', '.', '/', 0,   '*', 0,   ' ', 0,    0,    0,   0,   0,    0,   0,    0,
    0,   0,   0,   0,   0,   '7', '8',  '9', '-', '4', '5', '6', '+', '1', '2',  '3',  '0', '.', 0,    0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   0,   0,   0,   0,    0,    0,   0,   0,    0,   '\n', 0};

char key_char_table2[256] = {
    0,   0,   '!', '@', '#', '$', '%',  '^', '&', '*', '(', ')', '_', '+', 0,   0,   'Q', 'W', 'E', 'R', 'T',  'Y',
    'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,    '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M',  '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,    0,
    0,   0,   0,   0,   0,   '7', '8',  '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '\n', 0};

void complation(io::request_t *req, bool intr, u64 user_data)
{
    wait_queue_t *q = (wait_queue_t *)user_data;
    q->do_wake_up();
}

enum class switchable_key
{
    caps = 0,
    scroll,
    numlock,
};
using ::input::key;

bool get_key_switch_state(switchable_key k) { return key_switch_state.get_bit((u8)k); }

void set_key_switch_state(switchable_key k, bool enable)
{
    if (enable)
        key_switch_state.set_bit((u8)k);
    else
        key_switch_state.reset_bit((i8)k);
}

bool is_key_down(key k) { return key_down_state.get_bit((u64)k); }

bool is_ctrl_key_down() { return is_key_down(key::left_control) || is_key_down(key::right_control); }
bool is_alt_key_down() { return is_key_down(key::left_alt) || is_key_down(key::right_alt); }

bool is_fkey(key k) { return (k >= key::f1 && k <= key::f10) || (k >= key::f11 && k <= key::f12); }

void print_keyboard(io::keyboard_result_t &res, io::status_t &status, io::request_t *req,
                    freelibcxx::span<handle_t<fs::vfs::file>> tty_file_list)
{
    if (status.io_is_completion)
    {
        if (res.get.release)
        {
            key_down_state.reset_bit(res.get.key);
        }
        else
        {
            auto terms = term::get_terms();
            int idx = terms->term_index();
            // auto &term = terms->get(idx);
            auto tty = reinterpret_cast<dev::tty::tty_pseudo_t *>(tty_file_list[idx]->get_pseudo());

            key k = (key)res.get.key;
            if (k == key::capslock)
            {
                set_key_switch_state(switchable_key::caps, !get_key_switch_state(switchable_key::caps));
            }
            else if (k == key::scrolllock)
            {
                set_key_switch_state(switchable_key::scroll, !get_key_switch_state(switchable_key::scroll));
            }
            else if (k == key::numlock)
            {
                set_key_switch_state(switchable_key::numlock, !get_key_switch_state(switchable_key::numlock));
            }

            if (is_ctrl_key_down())
            {
                if (k == key::c)
                {
                    /// send sigint
                    auto proc = task::get_init_process();
                    proc->signal_pack.send(proc, task::signal::sigint, 0, 0, 0);
                }
                else if (k == key::d)
                {
                    tty->send_EOF();
                }
                else if (is_fkey(k) && is_alt_key_down())
                {
                    // control + alt + f1-f12
                    auto to_idx = k > key::f11 ? ((int)k - (int)key::f11) + 10 : (int)k - (int)key::f1;
                    if (to_idx < terms->total())
                    {
                        terms->switch_term(to_idx);
                    }
                }
            }
            else if (key_char_table[(u8)k] != 0)
            {
                byte d;
                if (is_key_down(key::left_shift) || is_key_down(key::right_shift))
                {
                    d = (byte)key_char_table2[(u8)k];
                }
                else
                {
                    d = (byte)key_char_table[(u8)k];
                }
                /// Capslock
                if (get_key_switch_state(switchable_key::caps))
                {
                    char c = (char)d;
                    if (c >= 'a' && c <= 'z')
                        c += 'A' - 'a';
                    else if (c >= 'A' && c <= 'Z')
                        c -= 'A' - 'a';

                    d = (byte)c;
                }
                if (d != (byte)0)
                {
                    tty->write_to_buffer(&d, 1, fs::rw_flags::override);
                }
            }

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
    freelibcxx::vector<handle_t<fs::vfs::file>> tty_file_list(memory::MemoryAllocatorV);
    freelibcxx::string ttyname(memory::MemoryAllocatorV, "/dev/tty/0");
    for (int i = 0; i < term::get_terms()->total(); i++)
    {
        ttyname.view()[ttyname.size() - 1] = '0' + i;
        auto input_file = fs::vfs::open(ttyname.data(), fs::vfs::global_root, fs::vfs::global_root, fs::mode::write, 0);
        tty_file_list.push_back(input_file);
    }

    key_down_state.reset_all();
    key_switch_state = 0;
    request.type = io::chain_number::keyboard;
    request.cmd_type = io::keyboard_request_t::command::get_key;
    request.final_completion_func = complation;
    request.status.io_is_completion = true;
    wait_queue_t input_wait_queue;
    request.completion_user_data = (u64)&input_wait_queue;
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
            input_wait_queue.do_wait([](u64 data) { return request.status.io_is_completion; }, 0);
        }
        print_keyboard(request.result, request.status, &request, tty_file_list.span());
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
    mreq.final_completion_func = complation;
    wait_queue_t input_wait_queue;
    mreq.completion_user_data = (u64)&input_wait_queue;
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
            input_wait_queue.do_wait([](u64 data) { return mreq.status.io_is_completion; }, 0);
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
