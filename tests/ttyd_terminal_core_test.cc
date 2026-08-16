#include "ttyd/terminal_core.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>

namespace
{
void test_canonical_echo_and_read()
{
    ttyd::terminal_core core;
    const std::uint8_t input[] = {'a', 'b', '\n'};
    std::size_t consumed = 0;
    assert(core.receive_input(input, sizeof(input), true, &consumed) == 3);
    assert(consumed == 3);
    assert(core.input_available() == 3);

    std::uint8_t output[16]{};
    std::size_t read = 0;
    assert(core.read_output(output, sizeof(output), true, &read) > 0);
    assert(read == 4);
    assert(std::memcmp(output, "ab\r\n", 4) == 0);

    std::uint8_t data[16]{};
    read = 0;
    assert(core.read_input(data, sizeof(data), true, &read) == 3);
    assert(std::memcmp(data, "ab\n", 3) == 0);
}

void test_erase_kill_and_eof()
{
    ttyd::terminal_core core;
    const std::uint8_t erase_input[] = {'a', 0x7f, 'b', '\n'};
    std::size_t consumed = 0;
    assert(core.receive_input(erase_input, sizeof(erase_input), true, &consumed) == 4);
    std::uint8_t data[16]{};
    std::size_t read = 0;
    assert(core.read_input(data, sizeof(data), true, &read) == 2);
    assert(std::memcmp(data, "b\n", 2) == 0);

    ttyd::terminal_core eof_core;
    const std::uint8_t eof = 0x04;
    assert(eof_core.receive_input(&eof, 1, true, &consumed) == 1);
    read = 0;
    assert(eof_core.read_input(data, sizeof(data), true, &read) == 0);
}

void test_canonical_reads_preserve_record_boundaries_and_eof_order()
{
    ttyd::terminal_core core;
    const std::uint8_t input[] = {'a', '\n', 0x04, 'b', '\n'};
    std::size_t consumed = 0;
    assert(core.receive_input(input, sizeof(input), true, &consumed) == static_cast<int>(sizeof(input)));

    std::uint8_t data[16]{};
    std::size_t read = 0;
    assert(core.read_input(data, sizeof(data), true, &read) == 2);
    assert(read == 2);
    assert(std::memcmp(data, "a\n", 2) == 0);

    assert(core.read_input(data, sizeof(data), true, &read) == 0);

    assert(core.read_input(data, sizeof(data), true, &read) == 2);
    assert(read == 2);
    assert(std::memcmp(data, "b\n", 2) == 0);
}

void test_raw_nonblocking_and_queue_full()
{
    ttyd::terminal_core core;
    auto termios = core.get_termios();
    termios.local_flags &= ~(ttyd::termios_lflag::icanon | ttyd::termios_lflag::echo);
    core.set_termios(termios);

    std::uint8_t data[16]{};
    std::size_t read = 0;
    assert(core.read_input(data, sizeof(data), true, &read) == -EAGAIN);

    std::uint8_t chunk[ttyd::terminal_core::queue_capacity]{};
    std::memset(chunk, 'x', sizeof(chunk));
    std::size_t consumed = 0;
    assert(core.receive_input(chunk, sizeof(chunk), true, &consumed) == static_cast<int>(sizeof(chunk)));
    const std::uint8_t extra = 'y';
    consumed = 0;
    assert(core.receive_input(&extra, 1, true, &consumed) == -EAGAIN);
    assert(core.input_available() == sizeof(chunk));

    std::uint8_t output[8]{};
    read = 0;
    assert(core.read_input(output, sizeof(output), true, &read) == 8);
    assert(core.input_available() == sizeof(chunk) - 8);
}

void test_output_transform_flush_and_hangup()
{
    ttyd::terminal_core core;
    const std::uint8_t input[] = {'a', '\n', 'b'};
    std::size_t written = 0;
    assert(core.write_output(input, sizeof(input), true, &written) == 3);
    std::uint8_t output[8]{};
    std::size_t read = 0;
    assert(core.read_output(output, sizeof(output), true, &read) == 4);
    assert(std::memcmp(output, "a\r\nb", 4) == 0);

    core.flush(ttyd::tty_flush::both);
    assert(core.input_available() == 0);
    assert(core.output_available() == 0);

    const std::uint8_t byte = 'z';
    std::size_t consumed = 0;
    assert(core.receive_input(&byte, 1, true, &consumed) == 1);
    core.hangup_master();
    std::uint8_t data[8]{};
    // A closed master is end-of-file on the slave side, not an I/O error.
    assert(core.read_input(data, sizeof(data), true, &read) == 0);
    assert(core.write_output(&byte, 1, true, &written) == -EIO);

    ttyd::terminal_core output_core;
    output_core.hangup_slave();
    assert(output_core.read_output(data, sizeof(data), true, &read) == -EIO);
}

void test_hangup_delivers_eof_after_drain()
{
    ttyd::terminal_core core;
    auto termios = core.get_termios();
    termios.local_flags &= ~(ttyd::termios_lflag::icanon | ttyd::termios_lflag::echo);
    core.set_termios(termios);

    const std::uint8_t pending[] = {'a', 'b'};
    std::size_t consumed = 0;
    assert(core.receive_input(pending, sizeof(pending), true, &consumed) == 2);
    core.hangup_master();

    std::uint8_t data[4]{};
    std::size_t read = 0;
    assert(core.read_input(data, sizeof(data), true, &read) == 2);
    assert(std::memcmp(data, pending, sizeof(pending)) == 0);
    assert(core.read_input(data, sizeof(data), true, &read) == 0);
}

void test_veof_mid_line_is_literal_data()
{
    ttyd::terminal_core core;
    const std::uint8_t input[] = {'a', 0x04, 'b', '\n'};
    std::size_t consumed = 0;
    assert(core.receive_input(input, sizeof(input), true, &consumed) == static_cast<int>(sizeof(input)));

    std::uint8_t data[8]{};
    std::size_t read = 0;
    assert(core.read_input(data, sizeof(data), true, &read) == 4);
    assert(std::memcmp(data,
                       "a\x04"
                       "b\n",
                       4) == 0);
}

void test_i_exten_gates_erase_and_kill()
{
    ttyd::terminal_core core;
    auto termios = core.get_termios();
    termios.local_flags &= ~ttyd::termios_lflag::ixten;
    core.set_termios(termios);

    const std::uint8_t input[] = {'a', 0x7f, 0x15, 'b', '\n'};
    std::size_t consumed = 0;
    assert(core.receive_input(input, sizeof(input), true, &consumed) == static_cast<int>(sizeof(input)));

    std::uint8_t data[8]{};
    std::size_t read = 0;
    assert(core.read_input(data, sizeof(data), true, &read) == 5);
    assert(std::memcmp(data,
                       "a\x7f\x15"
                       "b\n",
                       5) == 0);
}

void test_termios_rejects_unimplemented_flags()
{
    ttyd::terminal_core core;
    const auto original = core.get_termios();
    const auto generation = core.generation();

    auto unsupported_iflag = original;
    unsupported_iflag.input_flags |= 010000;
    assert(core.set_termios(unsupported_iflag) == -EINVAL);

    auto unsupported_lflag = original;
    unsupported_lflag.local_flags |= 0200000;
    assert(core.set_termios(unsupported_lflag) == -EINVAL);

    assert(core.get_termios().input_flags == original.input_flags);
    assert(core.get_termios().local_flags == original.local_flags);
    assert(core.generation() == generation);

    ttyd::termios raw{};
    assert(core.set_termios(raw) == 0);
    assert(core.get_termios().input_flags == 0);
    assert(core.get_termios().local_flags == 0);
}

void test_reopening_a_peer_clears_only_its_transient_hangup()
{
    const std::uint8_t byte = 'z';
    std::size_t count = 0;
    std::uint8_t data{};

    ttyd::terminal_core master;
    master.hangup_master();
    assert(master.write_output(&byte, 1, true, &count) == -EIO);
    master.open_master();
    assert(master.write_output(&byte, 1, true, &count) == 1);

    ttyd::terminal_core slave;
    slave.hangup_slave();
    assert((slave.master_poll_events() & ttyd::tty_poll::hangup) != 0);
    slave.open_slave();
    assert((slave.master_poll_events() & ttyd::tty_poll::hangup) == 0);
    assert(slave.write_output(&byte, 1, true, &count) == 1);
    assert(slave.read_output(&data, 1, true, &count) == 1);
    assert(data == byte);
}

void test_noncanonical_vmin_vtime_modes()
{
    ttyd::terminal_core immediate;
    auto termios = immediate.get_termios();
    termios.local_flags &= ~(ttyd::termios_lflag::icanon | ttyd::termios_lflag::echo);
    termios.control_chars[ttyd::termios_cc::vmin] = 0;
    termios.control_chars[ttyd::termios_cc::vtime] = 1;
    immediate.set_termios(termios);

    std::uint8_t data[8]{};
    std::size_t read = 0;
    assert(immediate.read_input(data, sizeof(data), true, &read) == -EAGAIN);
    assert(immediate.read_input(data, sizeof(data), true, &read, true) == 0);

    const std::uint8_t byte = 'x';
    std::size_t consumed = 0;
    assert(immediate.receive_input(&byte, 1, true, &consumed) == 1);
    assert(immediate.read_input(data, sizeof(data), true, &read) == 1);
    assert(data[0] == byte);

    ttyd::terminal_core interbyte;
    termios = interbyte.get_termios();
    termios.local_flags &= ~(ttyd::termios_lflag::icanon | ttyd::termios_lflag::echo);
    termios.control_chars[ttyd::termios_cc::vmin] = 3;
    termios.control_chars[ttyd::termios_cc::vtime] = 1;
    interbyte.set_termios(termios);
    const std::uint8_t partial[] = {'a', 'b'};
    assert(interbyte.receive_input(partial, sizeof(partial), true, &consumed) == 2);
    assert(interbyte.read_input(data, sizeof(data), true, &read) == -EAGAIN);
    assert(interbyte.read_input(data, sizeof(data), true, &read, true) == 2);
    assert(std::memcmp(data, partial, sizeof(partial)) == 0);
}

void test_generation_changes()
{
    ttyd::terminal_core core;
    const auto initial = core.generation();
    const std::uint8_t byte = 'a';
    std::size_t consumed = 0;
    (void)core.receive_input(&byte, 1, true, &consumed);
    assert(core.generation() != initial);
}

void test_external_state_changes_generation()
{
    ttyd::terminal_core core;
    const auto initial = core.generation();
    core.notify_external_change();
    assert(core.generation() != initial);
    const auto changed = core.generation();
    core.notify_external_change();
    assert(core.generation() != changed);
}

void test_echo_backpressure_never_drops_input_echo()
{
    ttyd::terminal_core core;
    std::uint8_t fill[ttyd::terminal_core::queue_capacity]{};
    std::memset(fill, 'x', sizeof(fill));
    std::size_t written = 0;
    assert(core.write_output(fill, sizeof(fill), true, &written) == static_cast<int>(sizeof(fill)));

    const std::uint8_t input = 'a';
    std::size_t consumed = 0;
    assert(core.receive_input(&input, 1, true, &consumed) == -EAGAIN);
    assert(consumed == 0);

    std::uint8_t drained{};
    std::size_t read = 0;
    assert(core.read_output(&drained, 1, true, &read) == 1);
    assert(core.receive_input(&input, 1, true, &consumed) == 1);
    assert(consumed == 1);
    std::uint8_t echoed{};
    assert(core.read_output(&echoed, 1, true, &read) == 1);
    assert(echoed == 'x');
}

void test_shutdown_input_is_one_way_eof()
{
    ttyd::terminal_core core;
    auto termios = core.get_termios();
    termios.local_flags &= ~(ttyd::termios_lflag::icanon | ttyd::termios_lflag::echo);
    core.set_termios(termios);

    const std::uint8_t input[] = {'a', 'b'};
    std::size_t consumed = 0;
    assert(core.receive_input(input, sizeof(input), true, &consumed) == 2);
    core.shutdown_input();

    std::uint8_t read_buffer[4]{};
    std::size_t read = 0;
    assert(core.read_input(read_buffer, sizeof(read_buffer), true, &read) == 2);
    assert(std::memcmp(read_buffer, input, sizeof(input)) == 0);
    assert(core.read_input(read_buffer, sizeof(read_buffer), true, &read) == 0);

    const std::uint8_t output = 'o';
    std::size_t written = 0;
    assert(core.write_output(&output, 1, true, &written) == 1);
    assert(core.read_output(read_buffer, sizeof(read_buffer), true, &read) == 1);
    assert(read_buffer[0] == output);

    consumed = 0;
    assert(core.receive_input(&output, 1, true, &consumed) == -EPIPE);
}

void test_output_transform_does_not_emit_partial_newline()
{
    ttyd::terminal_core core;
    std::uint8_t fill[ttyd::terminal_core::queue_capacity - 1]{};
    std::memset(fill, 'x', sizeof(fill));
    std::size_t written = 0;
    assert(core.write_output(fill, sizeof(fill), true, &written) == static_cast<int>(sizeof(fill)));

    const std::uint8_t newline = '\n';
    written = 0;
    assert(core.write_output(&newline, 1, true, &written) == -EAGAIN);
    assert(written == 0);
    assert(core.output_available() == sizeof(fill));

    std::uint8_t byte{};
    std::size_t read = 0;
    assert(core.read_output(&byte, 1, true, &read) == 1);
    assert(core.write_output(&newline, 1, true, &written) == 1);
    assert(core.output_available() == sizeof(fill) + 1);
}

void test_blocking_write_reports_committed_prefix()
{
    ttyd::terminal_core core;
    auto termios = core.get_termios();
    termios.local_flags &= ~(ttyd::termios_lflag::icanon | ttyd::termios_lflag::echo);
    core.set_termios(termios);

    std::uint8_t input[ttyd::terminal_core::queue_capacity + 1]{};
    std::size_t consumed = 0;
    assert(core.receive_input(input, sizeof(input), false, &consumed) == -EAGAIN);
    assert(consumed == ttyd::terminal_core::queue_capacity);
}

void test_zero_length_io_validates_without_mutation()
{
    ttyd::terminal_core core;
    const auto initial_generation = core.generation();
    std::size_t count = 123;
    std::uint8_t byte = 0;
    assert(core.receive_input(&byte, 0, true, &count) == 0);
    assert(count == 0);
    assert(core.read_input(&byte, 0, true, &count) == 0);
    assert(count == 0);
    assert(core.write_output(&byte, 0, true, &count) == 0);
    assert(count == 0);
    assert(core.read_output(&byte, 0, true, &count) == 0);
    assert(count == 0);
    assert(core.generation() == initial_generation);
}

void test_ixon_stop_start_controls_output_readiness()
{
    ttyd::terminal_core core;
    const std::uint8_t stop = 0x13;
    const std::uint8_t start = 0x11;
    std::size_t consumed = 0;
    assert(core.receive_input(&stop, 1, true, &consumed) == 1);
    assert(core.output_paused());
    assert((core.output_poll_events() & ttyd::tty_poll::writable) == 0);

    const std::uint8_t data = 'x';
    std::size_t written = 0;
    assert(core.write_output(&data, 1, true, &written) == -EAGAIN);
    assert(written == 0);

    assert(core.receive_input(&start, 1, true, &consumed) == 1);
    assert(!core.output_paused());
    assert((core.output_poll_events() & ttyd::tty_poll::writable) != 0);
    assert(core.write_output(&data, 1, true, &written) == 1);
}

void test_isig_flushes_input_and_output_unless_noflsh()
{
    ttyd::terminal_core core;
    const std::uint8_t data = 'x';
    std::size_t consumed = 0;
    assert(core.receive_input(&data, 1, true, &consumed) == 1);
    std::size_t written = 0;
    assert(core.write_output(&data, 1, true, &written) == 1);

    const std::uint8_t interrupt = 3;
    assert(core.receive_input(&interrupt, 1, true, &consumed) == 1);
    assert(core.input_available() == 0);
    assert(core.output_available() == 0);

    auto attributes = core.get_termios();
    attributes.local_flags |= ttyd::termios_lflag::noflsh;
    core.set_termios(attributes);
    assert(core.receive_input(&data, 1, true, &consumed) == 1);
    assert(core.write_output(&data, 1, true, &written) == 1);
    assert(core.receive_input(&interrupt, 1, true, &consumed) == 1);
    assert(core.input_available() != 0 || core.output_available() != 0);
}

void test_send_break_is_explicitly_unsupported()
{
    ttyd::terminal_core core;
    const auto initial_generation = core.generation();
    assert(core.send_break(0) == -ENOTSUP);
    assert(core.generation() == initial_generation);
}

void test_explicit_flow_controls_share_readiness_state()
{
    ttyd::terminal_core core;
    assert(core.set_flow(0) == 0);
    assert(core.output_paused());
    assert((core.output_poll_events() & ttyd::tty_poll::writable) == 0);
    assert(core.set_flow(1) == 0);
    assert(!core.output_paused());
    assert(core.set_flow(4) == -EINVAL);
}

void test_directional_readiness_tracks_each_endpoint()
{
    ttyd::terminal_core core;
    auto master = core.master_poll_events();
    auto slave = core.slave_poll_events();
    assert((master & ttyd::tty_poll::writable) != 0);
    assert((master & ttyd::tty_poll::readable) == 0);
    assert((slave & ttyd::tty_poll::writable) != 0);
    assert((slave & ttyd::tty_poll::readable) == 0);

    const std::uint8_t line[] = {'x'};
    std::size_t consumed = 0;
    assert(core.receive_input(line, sizeof(line), true, &consumed) == 1);
    assert((core.slave_poll_events() & ttyd::tty_poll::readable) == 0);
    const std::uint8_t newline = '\n';
    assert(core.receive_input(&newline, 1, true, &consumed) == 1);
    assert((core.slave_poll_events() & ttyd::tty_poll::readable) != 0);

    std::uint8_t output = 'o';
    std::size_t written = 0;
    assert(core.write_output(&output, 1, true, &written) == 1);
    assert((core.master_poll_events() & ttyd::tty_poll::readable) != 0);
    assert((core.slave_poll_events() & ttyd::tty_poll::writable) != 0);

    core.send_eof();
    assert((core.slave_poll_events() & ttyd::tty_poll::readable) != 0);
    core.hangup_master();
    assert((core.slave_poll_events() & ttyd::tty_poll::hangup) != 0);
}
} // namespace

int run_ttyd_terminal_core_tests()
{
    test_canonical_echo_and_read();
    test_erase_kill_and_eof();
    test_canonical_reads_preserve_record_boundaries_and_eof_order();
    test_raw_nonblocking_and_queue_full();
    test_noncanonical_vmin_vtime_modes();
    test_output_transform_flush_and_hangup();
    test_hangup_delivers_eof_after_drain();
    test_veof_mid_line_is_literal_data();
    test_i_exten_gates_erase_and_kill();
    test_termios_rejects_unimplemented_flags();
    test_reopening_a_peer_clears_only_its_transient_hangup();
    test_generation_changes();
    test_external_state_changes_generation();
    test_echo_backpressure_never_drops_input_echo();
    test_shutdown_input_is_one_way_eof();
    test_output_transform_does_not_emit_partial_newline();
    test_blocking_write_reports_committed_prefix();
    test_zero_length_io_validates_without_mutation();
    test_ixon_stop_start_controls_output_readiness();
    test_isig_flushes_input_and_output_unless_noflsh();
    test_send_break_is_explicitly_unsupported();
    test_explicit_flow_controls_share_readiness_state();
    test_directional_readiness_tracks_each_endpoint();
    return 0;
}
