#pragma once

#include <cstddef>
#include <cstdint>

namespace ttyd
{

struct termios
{
    std::uint32_t input_flags;
    std::uint32_t output_flags;
    std::uint32_t control_flags;
    std::uint32_t local_flags;
    std::uint8_t line;
    std::uint8_t control_chars[32];
    std::uint32_t input_baud;
    std::uint32_t output_baud;
};

struct winsize
{
    std::uint16_t rows;
    std::uint16_t columns;
    std::uint16_t x_pixels;
    std::uint16_t y_pixels;
};

enum class control_event : std::uint8_t
{
    interrupt,
    quit,
    suspend,
};

using control_event_handler = void (*)(control_event event, void *user_data);

namespace termios_cc
{
inline constexpr std::uint8_t vintr = 0;
inline constexpr std::uint8_t vquit = 1;
inline constexpr std::uint8_t verase = 2;
inline constexpr std::uint8_t vkill = 3;
inline constexpr std::uint8_t veof = 4;
inline constexpr std::uint8_t vtime = 5;
inline constexpr std::uint8_t vmin = 6;
inline constexpr std::uint8_t vstart = 8;
inline constexpr std::uint8_t vstop = 9;
inline constexpr std::uint8_t vsusp = 10;
} // namespace termios_cc

namespace termios_iflag
{
inline constexpr std::uint32_t inlcr = 0100;
inline constexpr std::uint32_t igncr = 0200;
inline constexpr std::uint32_t icrnl = 0400;
inline constexpr std::uint32_t ixon = 02000;
inline constexpr std::uint32_t ixany = 04000;
// Only flags the line discipline actually honors may enter the frozen
// protocol.  Accepting a flag and then ignoring it would let callers observe
// termios state that has no effect.
inline constexpr std::uint32_t supported = inlcr | igncr | icrnl | ixon | ixany;
} // namespace termios_iflag

namespace termios_oflag
{
inline constexpr std::uint32_t opost = 0001;
inline constexpr std::uint32_t onlcr = 0004;
inline constexpr std::uint32_t supported = opost | onlcr;
} // namespace termios_oflag

namespace termios_cflag
{
inline constexpr std::uint32_t cs8 = 0060;
inline constexpr std::uint32_t cread = 0200;
inline constexpr std::uint32_t supported = cs8 | cread;
} // namespace termios_cflag

namespace termios_lflag
{
inline constexpr std::uint32_t isig = 0001;
inline constexpr std::uint32_t icanon = 0002;
inline constexpr std::uint32_t echo = 0010;
inline constexpr std::uint32_t echoe = 0020;
inline constexpr std::uint32_t echok = 0040;
inline constexpr std::uint32_t echonl = 0100;
inline constexpr std::uint32_t noflsh = 0200;
inline constexpr std::uint32_t tostop = 0400;
inline constexpr std::uint32_t ixten = 0100000;
inline constexpr std::uint32_t supported = isig | icanon | echo | echoe | echok | echonl | noflsh | tostop | ixten;
} // namespace termios_lflag

namespace tty_flush
{
inline constexpr int input = 0;
inline constexpr int output = 1;
inline constexpr int both = 2;
} // namespace tty_flush

namespace tty_poll
{
inline constexpr std::uint32_t readable = 0x001;
inline constexpr std::uint32_t writable = 0x004;
inline constexpr std::uint32_t error = 0x008;
inline constexpr std::uint32_t hangup = 0x010;
} // namespace tty_poll

/// User-space PTY line discipline and byte queues. This object never blocks:
/// a service dispatcher retains pending responders and retries when readiness
/// changes. Every mutating operation bumps a generation for readiness watches.
class terminal_core
{
  public:
    static constexpr std::size_t queue_capacity = 4096;

    terminal_core();

    /// Master-side input: applies input transform/line discipline and queues
    /// bytes for the slave side. Returns consumed bytes, or a negative errno.
    int receive_input(const std::uint8_t *data, std::size_t size, bool nonblock, std::size_t *consumed);
    /// Slave-side input read.
    int read_input(std::uint8_t *data, std::size_t max_size, bool nonblock, std::size_t *read,
                   bool timeout_expired = false);
    /// Slave-side output write: applies output transform and queues master bytes.
    int write_output(const std::uint8_t *data, std::size_t size, bool nonblock, std::size_t *written);
    /// Master-side output read.
    int read_output(std::uint8_t *data, std::size_t max_size, bool nonblock, std::size_t *read);

    int flush(int queue);
    void send_eof();
    /// Half-close the master-to-slave direction. Already buffered input remains
    /// readable; subsequent slave reads observe EOF once it drains.
    void shutdown_input();
    void hangup_master();
    void hangup_slave();
    /// Clear a transient peer hangup when the first endpoint of that side is
    /// opened again. ttyd owns endpoint counts and calls these on 0 -> 1.
    void open_master();
    void open_slave();
    int send_break(std::uint32_t duration_ms);
    int set_flow(std::uint64_t action);

    termios get_termios() const;
    /// Reject attributes whose flags the line discipline does not implement
    /// instead of storing state that later reads would report as active.
    static int validate_termios(const termios &value);
    int set_termios(const termios &value);
    winsize get_winsize() const;
    void set_winsize(const winsize &value);
    void set_control_event_handler(control_event_handler handler, void *user_data = nullptr);

    std::uint64_t input_available() const;
    std::uint64_t output_available() const;
    bool output_paused() const { return output_paused_; }
    std::uint32_t input_poll_events() const;
    std::uint32_t output_poll_events() const;
    std::uint32_t master_poll_events() const;
    std::uint32_t slave_poll_events() const;
    std::uint64_t generation() const { return generation_; }
    /// Advance readiness generation for state owned by ttyd outside the byte
    /// queues (status flags, PTY lock and grant metadata).
    void notify_external_change();

    bool master_hung_up() const { return master_hung_up_; }
    bool slave_hung_up() const { return slave_hung_up_; }

  private:
    class ring
    {
      public:
        bool push(std::uint8_t value);
        bool pop(std::uint8_t &value);
        std::size_t available() const { return size_; }
        std::size_t free_space() const { return queue_capacity - size_; }
        bool empty() const { return size_ == 0; }
        bool full() const { return size_ == queue_capacity; }
        void clear();

      private:
        std::uint8_t data_[queue_capacity]{};
        std::size_t head_ = 0;
        std::size_t size_ = 0;
    };

    termios default_termios() const;
    bool is_canonical() const;
    bool enqueue_input_byte(std::uint8_t value);
    std::size_t enqueue_input_line();
    bool enqueue_input_record(std::size_t size);
    bool read_canonical_record(std::uint8_t *data, std::size_t max_size, std::size_t *read);
    void clear_input_records();
    bool try_enqueue_output_byte(std::uint8_t value);
    std::size_t echo_byte_size(std::uint8_t value) const;
    std::size_t echo_erase_size() const;
    std::size_t echo_kill_size() const;
    bool echo_byte(std::uint8_t value);
    bool echo_erase();
    bool echo_kill();
    void handle_control_event(std::uint8_t value);
    void bump_generation();

    ring input_ring_;
    ring output_ring_;
    std::uint8_t canonical_line_[queue_capacity]{};
    std::size_t canonical_size_ = 0;
    std::uint16_t input_record_sizes_[queue_capacity]{};
    std::size_t input_record_head_ = 0;
    std::size_t input_record_count_ = 0;

    termios termios_{};
    winsize winsize_{};
    control_event_handler control_handler_ = nullptr;
    void *control_handler_data_ = nullptr;
    std::uint64_t eof_count_ = 0;
    bool output_paused_ = false;
    bool input_shutdown_ = false;
    bool master_hung_up_ = false;
    bool slave_hung_up_ = false;
    std::uint64_t generation_ = 1;
};

} // namespace ttyd
