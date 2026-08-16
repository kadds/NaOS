#include "terminal_core.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ttyd
{

namespace
{
constexpr std::uint8_t default_intr = 3;
constexpr std::uint8_t default_quit = 28;
constexpr std::uint8_t default_erase = 127;
constexpr std::uint8_t default_kill = 21;
constexpr std::uint8_t default_eof = 4;
constexpr std::uint8_t default_start = 17;
constexpr std::uint8_t default_stop = 19;
constexpr std::uint8_t default_susp = 26;

} // namespace

bool terminal_core::ring::push(std::uint8_t value)
{
    if (full())
        return false;
    data_[(head_ + size_) % queue_capacity] = value;
    size_++;
    return true;
}

bool terminal_core::ring::pop(std::uint8_t &value)
{
    if (empty())
        return false;
    value = data_[head_];
    head_ = (head_ + 1) % queue_capacity;
    size_--;
    return true;
}

void terminal_core::ring::clear()
{
    head_ = 0;
    size_ = 0;
}

terminal_core::terminal_core()
    : termios_(default_termios())
    , winsize_{24, 80, 0, 0}
{
}

termios terminal_core::default_termios() const
{
    termios value{};
    value.input_flags = termios_iflag::icrnl | termios_iflag::ixon;
    value.output_flags = termios_oflag::opost | termios_oflag::onlcr;
    value.control_flags = termios_cflag::cs8 | termios_cflag::cread;
    value.local_flags = termios_lflag::isig | termios_lflag::icanon | termios_lflag::echo | termios_lflag::echoe |
                        termios_lflag::echok | termios_lflag::ixten;
    value.control_chars[termios_cc::vintr] = default_intr;
    value.control_chars[termios_cc::vquit] = default_quit;
    value.control_chars[termios_cc::verase] = default_erase;
    value.control_chars[termios_cc::vkill] = default_kill;
    value.control_chars[termios_cc::veof] = default_eof;
    value.control_chars[termios_cc::vtime] = 0;
    value.control_chars[termios_cc::vmin] = 1;
    value.control_chars[termios_cc::vstart] = default_start;
    value.control_chars[termios_cc::vstop] = default_stop;
    value.control_chars[termios_cc::vsusp] = default_susp;
    value.input_baud = 15;
    value.output_baud = 15;
    return value;
}

bool terminal_core::is_canonical() const { return (termios_.local_flags & termios_lflag::icanon) != 0; }

bool terminal_core::enqueue_input_byte(std::uint8_t value)
{
    if (!input_ring_.push(value))
        return false;
    bump_generation();
    return true;
}

std::size_t terminal_core::enqueue_input_line()
{
    if (canonical_size_ > input_ring_.free_space() || input_record_count_ == queue_capacity)
        return 0;
    std::size_t written = 0;
    for (std::size_t i = 0; i < canonical_size_; i++)
    {
        if (!input_ring_.push(canonical_line_[i]))
            break;
        written++;
    }
    if (written != canonical_size_ || !enqueue_input_record(written))
        return 0;
    canonical_size_ = 0;
    bump_generation();
    return written;
}

bool terminal_core::enqueue_input_record(std::size_t size)
{
    if (size > queue_capacity || input_record_count_ == queue_capacity)
        return false;
    input_record_sizes_[(input_record_head_ + input_record_count_) % queue_capacity] = static_cast<std::uint16_t>(size);
    input_record_count_++;
    return true;
}

bool terminal_core::read_canonical_record(std::uint8_t *data, std::size_t max_size, std::size_t *read)
{
    if (input_record_count_ == 0)
        return false;

    auto &remaining = input_record_sizes_[input_record_head_];
    const std::size_t count = std::min<std::size_t>(remaining, max_size);
    for (std::size_t i = 0; i < count; i++)
        (void)input_ring_.pop(data[i]);
    remaining = static_cast<std::uint16_t>(remaining - count);
    if (remaining == 0)
    {
        input_record_head_ = (input_record_head_ + 1) % queue_capacity;
        input_record_count_--;
    }
    *read = count;
    bump_generation();
    return true;
}

void terminal_core::clear_input_records()
{
    input_record_head_ = 0;
    input_record_count_ = 0;
}

bool terminal_core::try_enqueue_output_byte(std::uint8_t value)
{
    if (!output_ring_.push(value))
        return false;
    bump_generation();
    return true;
}

std::size_t terminal_core::echo_byte_size(std::uint8_t value) const
{
    if ((termios_.local_flags & termios_lflag::echo) == 0 &&
        (value != '\n' || (termios_.local_flags & termios_lflag::echonl) == 0))
        return 0;
    return value == '\n' && (termios_.output_flags & termios_oflag::onlcr) != 0 ? 2 : 1;
}

std::size_t terminal_core::echo_erase_size() const
{
    if ((termios_.local_flags & termios_lflag::echoe) == 0)
        return 0;
    return 3;
}

std::size_t terminal_core::echo_kill_size() const
{
    if ((termios_.local_flags & termios_lflag::echok) == 0)
        return 0;
    return 1;
}

bool terminal_core::echo_byte(std::uint8_t value)
{
    if (output_ring_.free_space() < echo_byte_size(value))
        return false;
    if (value == '\n' && (termios_.output_flags & termios_oflag::onlcr) != 0 && echo_byte_size(value) != 0)
        (void)try_enqueue_output_byte('\r');
    if (echo_byte_size(value) != 0)
        (void)try_enqueue_output_byte(value);
    return true;
}

bool terminal_core::echo_erase()
{
    if (output_ring_.free_space() < echo_erase_size())
        return false;
    if (echo_erase_size() == 0)
        return true;
    (void)try_enqueue_output_byte('\b');
    (void)try_enqueue_output_byte(' ');
    (void)try_enqueue_output_byte('\b');
    return true;
}

bool terminal_core::echo_kill()
{
    if (output_ring_.free_space() < echo_kill_size())
        return false;
    if (echo_kill_size() != 0)
        (void)try_enqueue_output_byte('\n');
    return true;
}

void terminal_core::handle_control_event(std::uint8_t value)
{
    if ((termios_.local_flags & termios_lflag::isig) == 0 || control_handler_ == nullptr)
        return;
    control_event event;
    if (value == termios_.control_chars[termios_cc::vintr])
        event = control_event::interrupt;
    else if (value == termios_.control_chars[termios_cc::vquit])
        event = control_event::quit;
    else if (value == termios_.control_chars[termios_cc::vsusp])
        event = control_event::suspend;
    else
        return;
    control_handler_(event, control_handler_data_);
}

int terminal_core::receive_input(const std::uint8_t *data, std::size_t size, bool nonblock, std::size_t *consumed)
{
    if ((data == nullptr && size != 0) || consumed == nullptr)
        return -EINVAL;
    *consumed = 0;
    if (slave_hung_up_)
        return -EIO;
    if (input_shutdown_)
        return -EPIPE;
    if (size == 0)
        return 0;

    for (; *consumed < size; (*consumed)++)
    {
        std::uint8_t value = data[*consumed];
        if (value == '\r')
        {
            if ((termios_.input_flags & termios_iflag::igncr) != 0)
                continue;
            if ((termios_.input_flags & termios_iflag::icrnl) != 0)
                value = '\n';
        }
        else if (value == '\n' && (termios_.input_flags & termios_iflag::inlcr) != 0)
        {
            value = '\r';
        }

        if ((termios_.input_flags & termios_iflag::ixon) != 0)
        {
            if (value == termios_.control_chars[termios_cc::vstop])
            {
                if (!output_paused_)
                {
                    output_paused_ = true;
                    bump_generation();
                }
                continue;
            }
            if (value == termios_.control_chars[termios_cc::vstart])
            {
                if (output_paused_)
                {
                    output_paused_ = false;
                    bump_generation();
                }
                continue;
            }
            if (output_paused_ && (termios_.input_flags & termios_iflag::ixany) != 0)
            {
                output_paused_ = false;
                bump_generation();
            }
        }

        const bool control =
            (termios_.local_flags & termios_lflag::isig) != 0 &&
            (value == termios_.control_chars[termios_cc::vintr] || value == termios_.control_chars[termios_cc::vquit] ||
             value == termios_.control_chars[termios_cc::vsusp]);
        if (control)
        {
            handle_control_event(value);
            if ((termios_.local_flags & termios_lflag::noflsh) == 0)
            {
                input_ring_.clear();
                canonical_size_ = 0;
                clear_input_records();
                eof_count_ = 0;
                output_ring_.clear();
                bump_generation();
            }
            continue;
        }

        if (is_canonical())
        {
            const bool extended = (termios_.local_flags & termios_lflag::ixten) != 0;
            if (extended && value == termios_.control_chars[termios_cc::verase])
            {
                if (canonical_size_ != 0)
                {
                    if (output_ring_.free_space() < echo_erase_size())
                        return nonblock ? (*consumed == 0 ? -EAGAIN : static_cast<int>(*consumed)) : -EAGAIN;
                    canonical_size_--;
                    (void)echo_erase();
                }
                continue;
            }
            if (extended && value == termios_.control_chars[termios_cc::vkill])
            {
                if (output_ring_.free_space() < echo_kill_size())
                    return nonblock ? (*consumed == 0 ? -EAGAIN : static_cast<int>(*consumed)) : -EAGAIN;
                canonical_size_ = 0;
                (void)echo_kill();
                continue;
            }
            // VEOF only commits an empty line when it is the first character of
            // a line.  Mid-line it is ordinary data and must not terminate the
            // record early.
            if (canonical_size_ == 0 && value == termios_.control_chars[termios_cc::veof])
            {
                if (!enqueue_input_record(0))
                    return nonblock ? (*consumed == 0 ? -EAGAIN : static_cast<int>(*consumed)) : -EAGAIN;
                bump_generation();
                continue;
            }
            if (canonical_size_ >= queue_capacity)
            {
                if (nonblock)
                    return *consumed == 0 ? -EAGAIN : static_cast<int>(*consumed);
                return -EAGAIN;
            }
            if (output_ring_.free_space() < echo_byte_size(value))
                return nonblock ? (*consumed == 0 ? -EAGAIN : static_cast<int>(*consumed)) : -EAGAIN;
            if (value == '\n' &&
                (canonical_size_ + 1 > input_ring_.free_space() || input_record_count_ == queue_capacity))
                return nonblock ? (*consumed == 0 ? -EAGAIN : static_cast<int>(*consumed)) : -EAGAIN;
            canonical_line_[canonical_size_++] = value;
            (void)echo_byte(value);
            if (value == '\n')
            {
                if (enqueue_input_line() == 0)
                {
                    if (nonblock)
                        return *consumed == 0 ? -EAGAIN : static_cast<int>(*consumed);
                    return -EAGAIN;
                }
            }
            continue;
        }

        if (input_ring_.full() || output_ring_.free_space() < echo_byte_size(value))
        {
            if (nonblock)
                return *consumed == 0 ? -EAGAIN : static_cast<int>(*consumed);
            return -EAGAIN;
        }
        (void)input_ring_.push(value);
        (void)echo_byte(value);
        bump_generation();
    }
    return static_cast<int>(*consumed);
}

int terminal_core::read_input(std::uint8_t *data, std::size_t max_size, bool nonblock, std::size_t *read,
                              bool timeout_expired)
{
    if ((data == nullptr && max_size != 0) || read == nullptr)
        return -EINVAL;
    *read = 0;
    if (max_size == 0)
        return 0;
    if (is_canonical() && read_canonical_record(data, max_size, read))
        return static_cast<int>(*read);

    if (input_ring_.empty())
    {
        if (eof_count_ != 0)
        {
            eof_count_--;
            bump_generation();
            return 0;
        }
        if (input_shutdown_)
        {
            return 0;
        }
        if (master_hung_up_)
        {
            // A closed master is end-of-file on the slave side once the
            // committed input has drained, not an I/O error.
            return 0;
        }
        if (!is_canonical() && termios_.control_chars[termios_cc::vtime] != 0 && timeout_expired)
            return 0;
        if (!is_canonical() && termios_.control_chars[termios_cc::vmin] == 0 &&
            termios_.control_chars[termios_cc::vtime] == 0)
            return 0;
        if (nonblock)
            return -EAGAIN;
        return -EAGAIN;
    }

    if (!is_canonical())
    {
        const std::size_t minimum = std::min<std::size_t>(termios_.control_chars[termios_cc::vmin], max_size);
        if (minimum != 0 && input_ring_.available() < minimum && !timeout_expired)
            return -EAGAIN;
    }

    std::size_t count = 0;
    std::uint8_t value = 0;
    while (count < max_size && input_ring_.pop(value))
        data[count++] = value;
    *read = count;
    bump_generation();
    return static_cast<int>(count);
}

int terminal_core::write_output(const std::uint8_t *data, std::size_t size, bool nonblock, std::size_t *written)
{
    if ((data == nullptr && size != 0) || written == nullptr)
        return -EINVAL;
    *written = 0;
    if (master_hung_up_)
        return -EIO;
    if (size == 0)
        return 0;
    if (output_paused_)
        return -EAGAIN;

    for (; *written < size; (*written)++)
    {
        std::uint8_t value = data[*written];
        const bool translate_newline = (termios_.output_flags & termios_oflag::opost) != 0 &&
                                       (termios_.output_flags & termios_oflag::onlcr) != 0 && value == '\n';
        const std::size_t required_space = translate_newline ? 2 : 1;
        if (output_ring_.free_space() < required_space)
        {
            if (nonblock)
                return *written == 0 ? -EAGAIN : static_cast<int>(*written);
            return -EAGAIN;
        }
        if (translate_newline)
            (void)try_enqueue_output_byte('\r');
        (void)try_enqueue_output_byte(value);
    }
    return static_cast<int>(*written);
}

int terminal_core::read_output(std::uint8_t *data, std::size_t max_size, bool nonblock, std::size_t *read)
{
    if ((data == nullptr && max_size != 0) || read == nullptr)
        return -EINVAL;
    *read = 0;
    if (max_size == 0)
        return 0;
    if (output_ring_.empty())
    {
        if (slave_hung_up_)
            return -EIO;
        if (nonblock)
            return -EAGAIN;
        return -EAGAIN;
    }

    std::size_t count = 0;
    std::uint8_t value = 0;
    while (count < max_size && output_ring_.pop(value))
        data[count++] = value;
    *read = count;
    bump_generation();
    return static_cast<int>(count);
}

int terminal_core::flush(int queue)
{
    if (queue < tty_flush::input || queue > tty_flush::both)
        return -EINVAL;
    if (queue == tty_flush::input || queue == tty_flush::both)
    {
        input_ring_.clear();
        canonical_size_ = 0;
        clear_input_records();
        eof_count_ = 0;
    }
    if (queue == tty_flush::output || queue == tty_flush::both)
        output_ring_.clear();
    bump_generation();
    return 0;
}

void terminal_core::send_eof()
{
    if (is_canonical())
    {
        if (!enqueue_input_record(0))
            return;
    }
    else
        eof_count_++;
    bump_generation();
}

void terminal_core::shutdown_input()
{
    input_shutdown_ = true;
    bump_generation();
}

void terminal_core::hangup_master()
{
    master_hung_up_ = true;
    bump_generation();
}

void terminal_core::hangup_slave()
{
    slave_hung_up_ = true;
    bump_generation();
}

void terminal_core::open_master()
{
    if (!master_hung_up_)
        return;
    master_hung_up_ = false;
    bump_generation();
}

void terminal_core::open_slave()
{
    if (!slave_hung_up_)
        return;
    slave_hung_up_ = false;
    bump_generation();
}

int terminal_core::send_break(std::uint32_t duration_ms)
{
    (void)duration_ms;
    if (slave_hung_up_)
        return -EIO;
    return -ENOTSUP;
}

int terminal_core::set_flow(std::uint64_t action)
{
    if (action > 3)
        return -EINVAL;
    const bool pause = action == 0 || action == 2;
    if (output_paused_ != pause)
    {
        output_paused_ = pause;
        bump_generation();
    }
    return 0;
}

termios terminal_core::get_termios() const { return termios_; }

int terminal_core::validate_termios(const termios &value)
{
    if ((value.input_flags & ~termios_iflag::supported) != 0 || (value.output_flags & ~termios_oflag::supported) != 0 ||
        (value.control_flags & ~termios_cflag::supported) != 0 || (value.local_flags & ~termios_lflag::supported) != 0)
        return -EINVAL;
    return 0;
}

int terminal_core::set_termios(const termios &value)
{
    if (validate_termios(value) != 0)
        return -EINVAL;
    termios_ = value;
    if ((termios_.input_flags & termios_iflag::ixon) == 0)
        output_paused_ = false;
    bump_generation();
    return 0;
}

winsize terminal_core::get_winsize() const { return winsize_; }

void terminal_core::set_winsize(const winsize &value)
{
    winsize_ = value;
    bump_generation();
}

void terminal_core::set_control_event_handler(control_event_handler handler, void *user_data)
{
    control_handler_ = handler;
    control_handler_data_ = user_data;
    bump_generation();
}

std::uint64_t terminal_core::input_available() const { return input_ring_.available(); }

std::uint64_t terminal_core::output_available() const { return output_ring_.available(); }

std::uint32_t terminal_core::input_poll_events() const
{
    std::uint32_t events = 0;
    if (!input_ring_.empty() || input_record_count_ != 0 || eof_count_ != 0 || master_hung_up_)
        events |= tty_poll::readable;
    if (!input_ring_.full() && !master_hung_up_)
        events |= tty_poll::writable;
    if (master_hung_up_)
        events |= tty_poll::error | tty_poll::hangup;
    return events;
}

std::uint32_t terminal_core::output_poll_events() const
{
    std::uint32_t events = 0;
    if (!output_ring_.empty() || slave_hung_up_)
        events |= tty_poll::readable;
    if (!output_ring_.full() && !master_hung_up_ && !output_paused_)
        events |= tty_poll::writable;
    if (slave_hung_up_)
        events |= tty_poll::error | tty_poll::hangup;
    return events;
}

std::uint32_t terminal_core::master_poll_events() const
{
    std::uint32_t events = 0;
    if (!output_ring_.empty() || slave_hung_up_)
        events |= tty_poll::readable;

    const bool echo_enabled =
        (termios_.local_flags & termios_lflag::echo) != 0 || (termios_.local_flags & termios_lflag::echonl) != 0;
    const std::size_t echo_space = !echo_enabled ? 0 : ((termios_.output_flags & termios_oflag::onlcr) != 0 ? 2 : 1);
    const bool input_space =
        is_canonical() ? canonical_size_ < queue_capacity && input_record_count_ < queue_capacity : !input_ring_.full();
    if (input_space && !input_shutdown_ && !slave_hung_up_ && output_ring_.free_space() >= echo_space)
        events |= tty_poll::writable;
    if (slave_hung_up_)
        events |= tty_poll::error | tty_poll::hangup;
    return events;
}

std::uint32_t terminal_core::slave_poll_events() const
{
    std::uint32_t events = 0;
    if (!input_ring_.empty() || input_record_count_ != 0 || eof_count_ != 0 || input_shutdown_ || master_hung_up_)
        events |= tty_poll::readable;
    if (!output_ring_.full() && !master_hung_up_ && !output_paused_)
        events |= tty_poll::writable;
    if (master_hung_up_)
        events |= tty_poll::error | tty_poll::hangup;
    return events;
}

void terminal_core::bump_generation()
{
    generation_++;
    if (generation_ == 0)
        generation_ = 1;
}

void terminal_core::notify_external_change() { bump_generation(); }

} // namespace ttyd
