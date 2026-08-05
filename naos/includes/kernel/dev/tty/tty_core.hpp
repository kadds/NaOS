#pragma once

#include "freelibcxx/allocator.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/common.hpp"
#include "kernel/dev/tty/termios.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/lock.hpp"
#include "kernel/wait.hpp"
#include <atomic>

namespace dev::tty
{
namespace tty_poll
{
inline constexpr u32 readable = 0x001;
inline constexpr u32 writable = 0x004;
inline constexpr u32 error = 0x008;
inline constexpr u32 hangup = 0x010;
} // namespace tty_poll

/// Origin metadata for the single slave-to-master output stream. It is not a
/// second PTY direction; the metadata is only used by the legacy framebuffer
/// console to keep input echo editable while committing real output.
enum class tty_output_source : u8
{
    normal,
    echo,
};

enum class control_event : u8
{
    interrupt,
    quit,
    suspend,
};

using control_event_handler = void (*)(control_event event, group_id foreground_group, u64 user_data);

class tty_core final
{
  public:
    explicit tty_core(u64 input_buffer_size = 4096, u64 output_buffer_size = 4096,
                      freelibcxx::Allocator *allocator = nullptr);

    tty_core(const tty_core &) = delete;
    tty_core &operator=(const tty_core &) = delete;

    i64 receive_input(const byte *data, u64 size, flag_t flags = 0);
    i64 read_input(byte *data, u64 max_size, flag_t flags = 0);
    i64 write_output(const byte *data, u64 size, flag_t flags = 0);
    i64 read_output(byte *data, u64 max_size, flag_t flags = 0, tty_output_source *sources = nullptr);

    void send_eof();
    void hangup_master();
    void hangup_slave();

    termios_t get_termios() const;
    void set_termios(const termios_t &termios);
    winsize_t get_winsize() const;
    void set_winsize(const winsize_t &winsize);
    void set_control_event_handler(control_event_handler handler, u64 user_data = 0);
    void set_foreground_process_group(group_id process_group);
    group_id foreground_process_group() const;
    void set_session_id(::session_id session);
    ::session_id session_id() const;

    void flush_input();
    void flush_output();

    u32 input_poll_events() const;
    u32 output_poll_events() const;
    bool input_readable() const;
    bool output_readable() const;

    bool master_hung_up() const { return master_hung_up_; }
    bool slave_hung_up() const { return slave_hung_up_; }

  private:
    struct input_settings
    {
        termios_t termios;
        control_event_handler handler;
        u64 handler_data;
        group_id foreground_group;
    };

    static freelibcxx::Allocator *select_allocator(freelibcxx::Allocator *allocator);
    static u64 normalize_buffer_size(u64 size);

    input_settings get_input_settings() const;
    bool is_canonical(const termios_t &termios) const;
    bool input_ready_for_read(const termios_t &termios) const;
    bool enqueue_input_byte(byte value);
    u64 enqueue_input_line();
    bool try_enqueue_output_byte(byte value, tty_output_source source);
    bool enqueue_output_byte(byte value, flag_t flags, u64 bytes_done, tty_output_source source);
    bool output_space_available() const;
    void echo_byte(byte value, const termios_t &termios);
    void echo_erase(const termios_t &termios);
    void echo_kill(const termios_t &termios);
    void handle_control_event(u8 value, const input_settings &settings);

    freelibcxx::circular_buffer<byte> input_buffer_;
    freelibcxx::circular_buffer<byte> output_buffer_;
    freelibcxx::circular_buffer<tty_output_source> output_sources_;
    freelibcxx::vector<byte> canonical_line_;

    mutable lock::spinlock_t config_lock_;
    lock::spinlock_t input_lock_;
    lock::spinlock_t output_lock_;
    lock::spinlock_t line_lock_;
    task::wait_queue_t input_wait_queue_;
    task::wait_queue_t output_wait_queue_;

    termios_t termios_;
    winsize_t winsize_;
    control_event_handler control_handler_;
    u64 control_handler_data_;
    std::atomic<group_id> foreground_process_group_;
    std::atomic<::session_id> session_id_;
    std::atomic<u64> input_available_;
    std::atomic<u64> input_free_;
    std::atomic<u64> output_available_;
    std::atomic<u64> output_free_;
    std::atomic<u64> eof_count_;
    std::atomic_bool master_hung_up_;
    std::atomic_bool slave_hung_up_;
};

/// Master-side view of a tty core: write injects input into the slave path and
/// read consumes output produced by the slave path.
class tty_master final
{
  public:
    explicit tty_master(tty_core &core)
        : core_(core)
    {
    }

    i64 write(const byte *data, u64 size, flag_t flags = 0) { return core_.receive_input(data, size, flags); }
    i64 read(byte *data, u64 max_size, flag_t flags = 0, tty_output_source *sources = nullptr)
    {
        return core_.read_output(data, max_size, flags, sources);
    }

  private:
    tty_core &core_;
};
} // namespace dev::tty
