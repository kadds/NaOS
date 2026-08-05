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

void handle_tty_control_event(dev::tty::control_event event, group_id foreground_group, u64 user_data)
{
    (void)user_data;
    signal_num_t signal_number = signal::sigint;
    switch (event)
    {
        case dev::tty::control_event::interrupt:
            signal_number = signal::sigint;
            break;
        case dev::tty::control_event::quit:
            signal_number = signal::sigquit;
            break;
        case dev::tty::control_event::suspend:
            signal_number = signal::sigstop;
            break;
    }

    if (foreground_group == 0)
        return;

    // The init process is the session supervisor. It must never be selected
    // as a TTY control-event fallback target: exiting it is a kernel-fatal
    // condition. Interactive shells establish their own process group before
    // accepting commands; until then, ignore the control event safely.
    auto *init_process = task::get_init_process();
    if (init_process != nullptr && init_process->process_group_id == foreground_group)
        return;

    task::send_signal_to_process_group(foreground_group, signal_number);
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

struct terminal_key_sequence
{
    const char *data;
    u64 size;
};

terminal_key_sequence get_terminal_key_sequence(key k)
{
    switch (k)
    {
        case key::cur_up:
            return {"\x1b[A", 3};
        case key::cur_down:
            return {"\x1b[B", 3};
        case key::cur_right:
            return {"\x1b[C", 3};
        case key::cur_left:
            return {"\x1b[D", 3};
        case key::home:
            return {"\x1b[H", 3};
        case key::end:
            return {"\x1b[F", 3};
        case key::insert:
            return {"\x1b[2~", 4};
        case key::delete_key:
            return {"\x1b[3~", 4};
        case key::page_up:
            return {"\x1b[5~", 4};
        case key::page_down:
            return {"\x1b[6~", 4};
        default:
            return {nullptr, 0};
    }
}

bool write_tty_input(dev::tty::tty_pseudo_t *tty, const byte *data, u64 size)
{
    u64 offset = 0;
    while (offset < size)
    {
        const i64 result = tty->write_to_buffer(data + offset, size - offset, fs::rw_flags::override);
        if (result <= 0)
            return false;
        offset += static_cast<u64>(result);
    }
    return true;
}

bool write_terminal_key(dev::tty::tty_pseudo_t *tty, key k)
{
    const auto sequence = get_terminal_key_sequence(k);
    if (sequence.data == nullptr)
        return false;
    return write_tty_input(tty, reinterpret_cast<const byte *>(sequence.data), sequence.size);
}

bool write_control_key(dev::tty::tty_pseudo_t *tty, key k)
{
    const char character = key_char_table[(u8)k];
    if (character < 'a' || character > 'z')
        return false;
    const byte control = static_cast<byte>(character - 'a' + 1);
    return write_tty_input(tty, &control, 1);
}

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
            if (idx < 0 || static_cast<u64>(idx) >= tty_file_list.size() || !tty_file_list[idx])
            {
                trace::warning("keyboard event has no target tty, terminal=", idx, " tty_count=", tty_file_list.size());
                io::finish_io_request(req);
                return;
            }
            auto tty = reinterpret_cast<dev::tty::tty_pseudo_t *>(tty_file_list[idx]->get_pseudo());
            if (tty == nullptr)
            {
                trace::warning("keyboard event target is not a tty, terminal=", idx);
                io::finish_io_request(req);
                return;
            }

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

            if (is_ctrl_key_down() && is_alt_key_down() && (k == key::f1 || k == key::f12))
            {
                const auto to_idx = k == key::f12 ? term::terminal_manager::kernel_console_index
                                                  : term::terminal_manager::user_terminal_index;
                if (terms->valid_index(to_idx))
                {
                    terms->switch_term(to_idx);
                }
            }
            else if (is_ctrl_key_down())
            {
                write_control_key(tty, k);
            }
            else if (!write_terminal_key(tty, k) && key_char_table[(u8)k] != 0)
            {
                byte d;
                // Terminals use DEL for VERASE.  The PS/2 key is not a
                // printable shifted character, so keep it at 0x7f even
                // when Shift is held.
                if (k == key::backspace)
                {
                    d = static_cast<byte>(0x7f);
                }
                else if (is_key_down(key::left_shift) || is_key_down(key::right_shift))
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
                    write_tty_input(tty, &d, 1);
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
    const char *tty_names[] = {"/dev/console", "/dev/tty0"};
    for (const auto *tty_name : tty_names)
    {
        auto input_file = fs::vfs::open(tty_name, fs::vfs::global_root, fs::vfs::global_root, fs::mode::write, 0);
        tty_file_list.push_back(input_file);
        if (input_file)
        {
            auto *tty = reinterpret_cast<dev::tty::tty_pseudo_t *>(input_file->get_pseudo());
            if (tty != nullptr)
                tty->core().set_control_event_handler(handle_tty_control_event);
        }
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
            input_wait_queue.do_wait([](u64 data) { return request.status.io_is_completion.load(); }, 0);
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
            input_wait_queue.do_wait([](u64 data) { return mreq.status.io_is_completion.load(); }, 0);
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
