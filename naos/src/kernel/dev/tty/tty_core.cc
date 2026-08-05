#include "kernel/dev/tty/tty_core.hpp"

#include "kernel/ipc/channel.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"

namespace dev::tty
{
namespace
{
constexpr u8 default_intr = 3;
constexpr u8 default_quit = 28;
constexpr u8 default_erase = 127;
constexpr u8 default_kill = 21;
constexpr u8 default_eof = 4;
constexpr u8 default_susp = 26;

termios_t default_termios()
{
    termios_t value{};
    value.c_iflag = termios_iflag::icrnl | termios_iflag::ixon;
    value.c_oflag = termios_oflag::opost | termios_oflag::onlcr;
    value.c_cflag = termios_cflag::cs8 | termios_cflag::cread;
    value.c_lflag = termios_lflag::isig | termios_lflag::icanon | termios_lflag::echo | termios_lflag::echoe |
                    termios_lflag::echok | termios_lflag::ixten;
    value.c_cc[termios_cc::vintr] = default_intr;
    value.c_cc[termios_cc::vquit] = default_quit;
    value.c_cc[termios_cc::verase] = default_erase;
    value.c_cc[termios_cc::vkill] = default_kill;
    value.c_cc[termios_cc::veof] = default_eof;
    value.c_cc[termios_cc::vtime] = 0;
    value.c_cc[termios_cc::vmin] = 1;
    value.c_cc[termios_cc::vsusp] = default_susp;
    value.ibaud = 15;
    value.obaud = 15;
    return value;
}

} // namespace

freelibcxx::Allocator *tty_core::select_allocator(freelibcxx::Allocator *allocator)
{
    return allocator != nullptr ? allocator : memory::MemoryAllocatorV;
}

u64 tty_core::normalize_buffer_size(u64 size) { return size < 2 ? 2 : size; }

tty_core::tty_core(u64 input_buffer_size, u64 output_buffer_size, freelibcxx::Allocator *allocator)
    : input_buffer_(select_allocator(allocator), normalize_buffer_size(input_buffer_size))
    , output_buffer_(select_allocator(allocator), normalize_buffer_size(output_buffer_size))
    , output_sources_(select_allocator(allocator), normalize_buffer_size(output_buffer_size))
    , canonical_line_(select_allocator(allocator))
    , termios_(default_termios())
    , winsize_{24, 80, 0, 0}
    , control_handler_(nullptr)
    , control_handler_data_(0)
    , foreground_process_group_(0)
    , session_id_(0)
    , input_available_(0)
    , input_free_(input_buffer_.capacity() - 1)
    , output_available_(0)
    , output_free_(output_buffer_.capacity() - 1)
    , eof_count_(0)
    , master_hung_up_(false)
    , slave_hung_up_(false)
{
}

tty_core::input_settings tty_core::get_input_settings() const
{
    lock::spinlock_t &lock = const_cast<lock::spinlock_t &>(config_lock_);
    uctx::RawSpinLockUninterruptibleContext guard(lock);
    input_settings settings{termios_, control_handler_, control_handler_data_, foreground_process_group_.load()};
    return settings;
}

bool tty_core::is_canonical(const termios_t &termios) const { return (termios.c_lflag & termios_lflag::icanon) != 0; }

bool tty_core::input_ready_for_read(const termios_t &termios) const
{
    if (eof_count_.load() != 0 || master_hung_up_.load())
        return true;
    if (is_canonical(termios))
        return input_available_.load() != 0;
    return input_available_.load() != 0;
}

bool tty_input_wait_condition(u64 data)
{
    auto *tty = reinterpret_cast<tty_core *>(data);
    if (tty == nullptr)
        return true;
    return tty->input_readable() || tty->master_hung_up() || tty->slave_hung_up();
}

bool tty_input_space_wait_condition(u64 data)
{
    auto *tty = reinterpret_cast<tty_core *>(data);
    return tty == nullptr || (tty->input_poll_events() & tty_poll::writable) != 0;
}

bool tty_output_wait_condition(u64 data)
{
    auto *tty = reinterpret_cast<tty_core *>(data);
    return tty == nullptr || tty->output_readable() || tty->slave_hung_up();
}

bool tty_output_space_wait_condition(u64 data)
{
    auto *tty = reinterpret_cast<tty_core *>(data);
    return tty == nullptr || (tty->output_poll_events() & tty_poll::writable) != 0;
}

bool tty_core::enqueue_input_byte(byte value)
{
    uctx::RawSpinLockUninterruptibleContext guard(input_lock_);
    if (input_buffer_.full())
        return false;
    input_buffer_.write(value);
    input_available_.fetch_add(1);
    input_free_.fetch_sub(1);
    return true;
}

u64 tty_core::enqueue_input_line()
{
    u64 written = 0;
    uctx::RawSpinLockUninterruptibleContext guard(input_lock_);
    for (auto value : canonical_line_)
    {
        if (input_buffer_.full())
            break;
        input_buffer_.write(value);
        written++;
    }
    canonical_line_.clear();
    input_available_.fetch_add(written);
    input_free_.fetch_sub(written);
    return written;
}

bool tty_core::try_enqueue_output_byte(byte value, tty_output_source source)
{
    if (output_buffer_.full() || output_sources_.full())
        return false;
    output_buffer_.write(value);
    output_sources_.write(source);
    output_available_.fetch_add(1);
    output_free_.fetch_sub(1);
    return true;
}

bool tty_core::enqueue_output_byte(byte value, flag_t flags, u64 bytes_done, tty_output_source source)
{
    while (true)
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(output_lock_);
            if (try_enqueue_output_byte(value, source))
                return true;
        }
        if (flags & fs::rw_flags::no_block)
            return false;
        output_wait_queue_.do_wait(tty_output_space_wait_condition, reinterpret_cast<u64>(this));
    }
}

bool tty_core::output_space_available() const { return output_free_.load() != 0; }

void tty_core::echo_byte(byte value, const termios_t &termios)
{
    if ((termios.c_lflag & termios_lflag::echo) == 0 &&
        (static_cast<u8>(value) != '\n' || (termios.c_lflag & termios_lflag::echonl) == 0))
        return;

    {
        uctx::RawSpinLockUninterruptibleContext guard(output_lock_);
        if (static_cast<u8>(value) == '\n' && (termios.c_oflag & termios_oflag::onlcr))
            try_enqueue_output_byte(static_cast<byte>('\r'), tty_output_source::echo);
        try_enqueue_output_byte(value, tty_output_source::echo);
    }
    output_wait_queue_.do_wake_up();
}

void tty_core::echo_erase(const termios_t &termios)
{
    if ((termios.c_lflag & termios_lflag::echoe) == 0)
        return;
    {
        uctx::RawSpinLockUninterruptibleContext guard(output_lock_);
        try_enqueue_output_byte(static_cast<byte>('\b'), tty_output_source::echo);
        try_enqueue_output_byte(static_cast<byte>(' '), tty_output_source::echo);
        try_enqueue_output_byte(static_cast<byte>('\b'), tty_output_source::echo);
    }
    output_wait_queue_.do_wake_up();
}

void tty_core::echo_kill(const termios_t &termios)
{
    if ((termios.c_lflag & termios_lflag::echok) == 0)
        return;
    {
        uctx::RawSpinLockUninterruptibleContext guard(output_lock_);
        try_enqueue_output_byte(static_cast<byte>('\n'), tty_output_source::echo);
    }
    output_wait_queue_.do_wake_up();
}

void tty_core::handle_control_event(u8 value, const input_settings &settings)
{
    if ((settings.termios.c_lflag & termios_lflag::isig) == 0 || settings.handler == nullptr)
        return;

    control_event event;
    if (value == settings.termios.c_cc[termios_cc::vintr])
        event = control_event::interrupt;
    else if (value == settings.termios.c_cc[termios_cc::vquit])
        event = control_event::quit;
    else if (value == settings.termios.c_cc[termios_cc::vsusp])
        event = control_event::suspend;
    else
        return;
    settings.handler(event, settings.foreground_group, settings.handler_data);
}

i64 tty_core::receive_input(const byte *data, u64 size, flag_t flags)
{
    if (data == nullptr)
        return EINVAL;
    if (master_hung_up_.load())
        return EIO;

    u64 consumed = 0;
    for (; consumed < size; consumed++)
    {
        auto settings = get_input_settings();
        byte value = data[consumed];
        if (static_cast<u8>(value) == '\r')
        {
            if (settings.termios.c_iflag & termios_iflag::igncr)
                continue;
            if (settings.termios.c_iflag & termios_iflag::icrnl)
                value = static_cast<byte>('\n');
        }
        else if (static_cast<u8>(value) == '\n' && (settings.termios.c_iflag & termios_iflag::inlcr))
        {
            value = static_cast<byte>('\r');
        }

        bool control = (settings.termios.c_lflag & termios_lflag::isig) != 0 &&
                       (static_cast<u8>(value) == settings.termios.c_cc[termios_cc::vintr] ||
                        static_cast<u8>(value) == settings.termios.c_cc[termios_cc::vquit] ||
                        static_cast<u8>(value) == settings.termios.c_cc[termios_cc::vsusp]);
        if (control)
        {
            handle_control_event(static_cast<u8>(value), settings);
            continue;
        }

        bool input_full = false;
        bool handled = false;
        bool wake_input = false;
        {
            uctx::RawSpinLockUninterruptibleContext line_guard(line_lock_);
            if (is_canonical(settings.termios))
            {
                if (static_cast<u8>(value) == settings.termios.c_cc[termios_cc::verase])
                {
                    if (!canonical_line_.empty())
                    {
                        canonical_line_.pop_back();
                        echo_erase(settings.termios);
                    }
                    handled = true;
                }
                else if (static_cast<u8>(value) == settings.termios.c_cc[termios_cc::vkill])
                {
                    canonical_line_.clear();
                    echo_kill(settings.termios);
                    handled = true;
                }
                else if (static_cast<u8>(value) == settings.termios.c_cc[termios_cc::veof])
                {
                    if (canonical_line_.empty())
                        eof_count_.fetch_add(1);
                    else
                        enqueue_input_line();
                    handled = true;
                    wake_input = true;
                }
                else
                {
                    canonical_line_.push_back(value);
                    echo_byte(value, settings.termios);
                    if (static_cast<u8>(value) == '\n')
                        enqueue_input_line();
                }
            }
            else
            {
                {
                    uctx::RawSpinLockUninterruptibleContext input_guard(input_lock_);
                    if (input_buffer_.full())
                    {
                        input_full = true;
                    }
                    else
                    {
                        input_buffer_.write(value);
                        input_available_.fetch_add(1);
                        input_free_.fetch_sub(1);
                    }
                }
                if (!input_full)
                    echo_byte(value, settings.termios);
            }
        }
        if (input_full)
        {
            if (flags & fs::rw_flags::no_block)
                return consumed == 0 ? EAGAIN : static_cast<i64>(consumed);
            input_wait_queue_.do_wait(tty_input_space_wait_condition, reinterpret_cast<u64>(this));
            consumed--;
            continue;
        }
        if (handled)
        {
            if (wake_input)
            {
                input_wait_queue_.do_wake_up();
                naos::ipc::notify_channel_waiters();
            }
            continue;
        }
        input_wait_queue_.do_wake_up();
        naos::ipc::notify_channel_waiters();
    }
    return static_cast<i64>(consumed);
}

i64 tty_core::read_input(byte *data, u64 max_size, flag_t flags)
{
    if (data == nullptr || max_size == 0)
        return EINVAL;
    while (input_available_.load() == 0)
    {
        if (eof_count_.load() != 0)
        {
            eof_count_.fetch_sub(1);
            return 0;
        }
        if (master_hung_up_.load())
            return EIO;
        if (flags & fs::rw_flags::no_block)
            return EAGAIN;
        input_wait_queue_.do_wait(tty_input_wait_condition, reinterpret_cast<u64>(this));
    }

    u64 read;
    {
        uctx::RawSpinLockUninterruptibleContext guard(input_lock_);
        const u64 count = freelibcxx::min<u64>(max_size, input_buffer_.capacity_readable());
        read = input_buffer_.read(data, count);
        input_available_.fetch_sub(read);
        input_free_.fetch_add(read);
    }
    input_wait_queue_.do_wake_up();
    naos::ipc::notify_channel_waiters();
    return static_cast<i64>(read);
}

i64 tty_core::write_output(const byte *data, u64 size, flag_t flags)
{
    if (data == nullptr)
        return EINVAL;
    if (slave_hung_up_.load())
        return EIO;

    auto settings = get_input_settings();
    u64 written = 0;
    while (written < size)
    {
        byte value = data[written];
        if ((settings.termios.c_oflag & termios_oflag::opost) && static_cast<u8>(value) == '\n' &&
            (settings.termios.c_oflag & termios_oflag::onlcr))
        {
            if (!enqueue_output_byte(static_cast<byte>('\r'), flags, written, tty_output_source::normal))
                return written == 0 ? EAGAIN : static_cast<i64>(written);
        }
        if (!enqueue_output_byte(value, flags, written, tty_output_source::normal))
            return written == 0 ? EAGAIN : static_cast<i64>(written);
        written++;
    }
    output_wait_queue_.do_wake_up();
    naos::ipc::notify_channel_waiters();
    return static_cast<i64>(written);
}

i64 tty_core::read_output(byte *data, u64 max_size, flag_t flags, tty_output_source *sources)
{
    if (data == nullptr || max_size == 0)
        return EINVAL;
    while (output_available_.load() == 0)
    {
        if (slave_hung_up_.load())
            return EIO;
        if (flags & fs::rw_flags::no_block)
            return EAGAIN;
        output_wait_queue_.do_wait(tty_output_wait_condition, reinterpret_cast<u64>(this));
    }

    u64 read = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(output_lock_);
        const u64 count = freelibcxx::min<u64>(max_size, output_buffer_.capacity_readable());
        while (read < count)
        {
            output_buffer_.read(&data[read]);
            tty_output_source source;
            output_sources_.read(&source);
            if (sources != nullptr)
                sources[read] = source;
            read++;
        }
        output_available_.fetch_sub(read);
        output_free_.fetch_add(read);
    }
    output_wait_queue_.do_wake_up();
    naos::ipc::notify_channel_waiters();
    return static_cast<i64>(read);
}

void tty_core::send_eof()
{
    eof_count_.fetch_add(1);
    input_wait_queue_.do_wake_up();
    naos::ipc::notify_channel_waiters();
}

void tty_core::hangup_master()
{
    master_hung_up_.store(true);
    input_wait_queue_.do_wake_up();
    output_wait_queue_.do_wake_up();
    naos::ipc::notify_channel_waiters();
}

void tty_core::hangup_slave()
{
    slave_hung_up_.store(true);
    input_wait_queue_.do_wake_up();
    output_wait_queue_.do_wake_up();
    naos::ipc::notify_channel_waiters();
}

termios_t tty_core::get_termios() const
{
    auto *self = const_cast<tty_core *>(this);
    uctx::RawSpinLockUninterruptibleContext guard(self->config_lock_);
    auto value = self->termios_;
    return value;
}

void tty_core::set_termios(const termios_t &termios)
{
    uctx::RawSpinLockUninterruptibleContext guard(config_lock_);
    termios_ = termios;
}

winsize_t tty_core::get_winsize() const
{
    auto *self = const_cast<tty_core *>(this);
    uctx::RawSpinLockUninterruptibleContext guard(self->config_lock_);
    auto value = self->winsize_;
    return value;
}

void tty_core::set_winsize(const winsize_t &winsize)
{
    {
        uctx::RawSpinLockUninterruptibleContext guard(config_lock_);
        winsize_ = winsize;
    }
    if (const auto foreground_group = foreground_process_group(); foreground_group != 0)
        task::send_signal_to_process_group(foreground_group, task::signal::sigwinch);
}

void tty_core::set_control_event_handler(control_event_handler handler, u64 user_data)
{
    uctx::RawSpinLockUninterruptibleContext guard(config_lock_);
    control_handler_ = handler;
    control_handler_data_ = user_data;
}

void tty_core::set_foreground_process_group(group_id process_group) { foreground_process_group_.store(process_group); }

group_id tty_core::foreground_process_group() const { return foreground_process_group_.load(); }

void tty_core::set_session_id(::session_id session) { session_id_.store(session); }

::session_id tty_core::session_id() const { return session_id_.load(); }

void tty_core::flush_input()
{
    {
        uctx::RawSpinLockUninterruptibleContext guard(input_lock_);
        byte value;
        while (input_buffer_.read(&value))
        {
        }
        input_available_.store(0);
        input_free_.store(input_buffer_.capacity() - 1);
    }
    {
        uctx::RawSpinLockUninterruptibleContext guard(line_lock_);
        canonical_line_.clear();
    }
    input_wait_queue_.do_wake_up();
    naos::ipc::notify_channel_waiters();
}

void tty_core::flush_output()
{
    {
        uctx::RawSpinLockUninterruptibleContext guard(output_lock_);
        byte value;
        tty_output_source source;
        while (output_buffer_.read(&value))
        {
            output_sources_.read(&source);
        }
        output_available_.store(0);
        output_free_.store(output_buffer_.capacity() - 1);
    }
    output_wait_queue_.do_wake_up();
    naos::ipc::notify_channel_waiters();
}

u32 tty_core::input_poll_events() const
{
    u32 events = 0;
    if (input_available_.load() != 0 || eof_count_.load() != 0 || master_hung_up_.load())
        events |= tty_poll::readable;
    if (input_free_.load() != 0 && !master_hung_up_.load())
        events |= tty_poll::writable;
    if (master_hung_up_.load())
        events |= tty_poll::error | tty_poll::hangup;
    return events;
}

u32 tty_core::output_poll_events() const
{
    u32 events = 0;
    if (output_available_.load() != 0 || slave_hung_up_.load())
        events |= tty_poll::readable;
    if (output_free_.load() != 0 && !slave_hung_up_.load())
        events |= tty_poll::writable;
    if (slave_hung_up_.load())
        events |= tty_poll::error | tty_poll::hangup;
    return events;
}

bool tty_core::input_readable() const { return (input_poll_events() & tty_poll::readable) != 0; }

bool tty_core::output_readable() const { return (output_poll_events() & tty_poll::readable) != 0; }

} // namespace dev::tty
