#include <abi-bits/ioctls.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <naos/generated/system/TerminalManager.hpp>
#include <naos/generated/system/TerminalManager_client.hpp>
#include <naos/generated/system/TerminalMaster.hpp>
#include <naos/generated/system/TerminalMaster_client.hpp>
#include <naos/generated/system/TerminalSlave.hpp>
#include <naos/generated/system/TerminalSlave_client.hpp>
#include <naos/service_directory.hpp>
#include <naos/syscall.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

[[gnu::weak]] void *__dso_handle;
extern char **environ;

extern "C" {
int run_mlibc_tls_smoke();
int naos_take_terminal_driver_factory(na_handle_t *handle);
int naos_take_console_frontend(na_handle_t *handle);
int naos_take_input_event_source(na_handle_t *handle);
int ioctl(int fd, unsigned long request, ...);
int naos_native_spawn(pid_t *pid, const char *path, char *const argv[], char *const envp[]);
int naos_native_spawn_stdio(pid_t *pid, const char *path, char *const argv[], char *const envp[], int stdin_fd,
                            int stdout_fd, int stderr_fd);
int naos_native_spawn_stdio_deferred(pid_t *pid, na_handle_t *process, const char *path, char *const argv[],
                                     char *const envp[], int stdin_fd, int stdout_fd, int stderr_fd);
int naos_native_start_process(na_handle_t process);
int naos_native_spawn_with_terminal_factory(pid_t *pid, const char *path, char *const argv[], char *const envp[],
                                            na_handle_t factory_handle);
int naos_native_spawn_with_capabilities(pid_t *pid, const char *path, char *const argv[], char *const envp[],
                                        const na_bootstrap_capability_t *capabilities, uint32_t capability_count);
int naos_native_spawn_with_terminal_factory_and_service_manager(pid_t *pid, const char *path, char *const argv[],
                                                                char *const envp[], na_handle_t factory_handle);
}

void smoke_log(const char *message)
{
    _s_log(message);
    fputs(message, stdout);
    fflush(stdout);
}

void smoke_printf(const char *format, ...)
{
    char message[512]{};
    va_list arguments;
    va_start(arguments, format);
    const int result = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (result < 0)
    {
        smoke_log("smoke: output formatting failed\n");
        return;
    }
    smoke_log(message);
}

namespace
{
naoidl::native_transport make_native_transport()
{
    naoidl::native_transport_api api{};
    api.handle_close = [](void *, na_handle_t handle) { return static_cast<na_status_t>(_na_handle_close(handle)); };
    api.handle_get_info = [](void *, na_handle_t handle, na_handle_info_t *info) {
        return static_cast<na_status_t>(_na_handle_get_info(handle, info));
    };
    api.invoke_submit = [](void *, na_handle_t target, const na_submit_frame_t *frame, na_handle_t *invocation) {
        return static_cast<na_status_t>(_na_invoke_submit(target, frame, invocation));
    };
    api.invocation_take_result = [](void *, na_handle_t invocation, na_result_frame_t *frame) {
        return static_cast<na_status_t>(_na_invocation_take_result(invocation, frame));
    };
    return naoidl::native_transport(api);
}

int wait_invocation(na_handle_t invocation)
{
    na_wait_item_t item{invocation, NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED, 0};
    const auto status = _na_handle_wait_many(&item, 1, nullptr);
    return status == NA_STATUS_OK ? 0 : static_cast<int>(status);
}

void ttyd_smoke()
{
    smoke_log("smoke: connecting existing terminal manager\n");

    na_handle_t manager = NA_HANDLE_INVALID;
    int error = naos_service_connect_versioned("naos://system/terminal", &naos::system::TerminalManager::protocol_uuid,
                                               NA_PROTOCOL_RIGHT_INVOKE, naos::system::TerminalManager::revision,
                                               naos::system::TerminalManager::features, &manager);
    if (error != 0)
    {
        smoke_printf("smoke: connect terminal manager failed %d\n", error);
        smoke_log("smoke: connect terminal manager failed\n");
        return;
    }
    smoke_log("smoke: connected terminal manager\n");

    auto transport = make_native_transport();
    auto client = naos::system::TerminalManager::TerminalManagerClient(transport.async(), manager);
    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    naos::system::TerminalManager::create_pty_request request{};
    request.locked = 0;
    request.mode = 1 | 2;
    na_handle_t invocation = NA_HANDLE_INVALID;
    auto status = client.submit_create_pty(request, nullptr, 0, &invocation, wire, sizeof(wire));
    if (status != NA_STATUS_OK)
    {
        smoke_printf("smoke: create_pty submit failed %d\n", static_cast<int>(status));
        smoke_log("smoke: create_pty submit failed\n");
        (void)naos_handle_close(manager);
        return;
    }
    smoke_log("smoke: create_pty submitted\n");
    if (wait_invocation(invocation) != 0)
    {
        smoke_printf("smoke: create_pty wait failed\n");
        smoke_log("smoke: create_pty wait failed\n");
        (void)naos_handle_close(invocation);
        (void)naos_handle_close(manager);
        return;
    }
    smoke_log("smoke: create_pty completed\n");
    naos::system::TerminalManager::create_pty_response response{};
    na_handle_t resources[NA_CHANNEL_MAX_RESOURCES]{};
    na_result_frame_t result{};
    status =
        client.take_create_pty(invocation, response, wire, sizeof(wire), resources, NA_CHANNEL_MAX_RESOURCES, result);
    (void)naos_handle_close(invocation);
    if (status == NA_STATUS_OK && result.execution_outcome == NA_EXECUTION_NONE && result.protocol_error == 0 &&
        result.actual_resources == 2)
    {
        smoke_printf("smoke: ttyd create_pty ok number=%llu\n", static_cast<unsigned long long>(response.number));
        smoke_log("smoke: ttyd create_pty ok\n");

        const na_handle_t master_handle = resources[response.master.value];
        if (response.job_control.value < result.actual_resources)
            (void)naos_handle_close(resources[response.job_control.value]);
        auto master_client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master_handle);

        naos::system::TerminalManager::open_pty_slave_request open_request{};
        open_request.locator = response.slave_locator;
        open_request.mode = 1 | 2;
        na_handle_t open_invocation = NA_HANDLE_INVALID;
        status = client.submit_open_pty_slave(open_request, nullptr, 0, &open_invocation, wire, sizeof(wire));
        if (status != NA_STATUS_OK || wait_invocation(open_invocation) != 0)
        {
            smoke_log("smoke: open_pty_slave failed\n");
            (void)naos_handle_close(master_handle);
            (void)naos_handle_close(manager);
            return;
        }
        naos::system::TerminalManager::open_pty_slave_response open_response{};
        na_handle_t slave_resources[NA_CHANNEL_MAX_RESOURCES]{};
        na_result_frame_t open_result{};
        status = client.take_open_pty_slave(open_invocation, open_response, wire, sizeof(wire), slave_resources,
                                            NA_CHANNEL_MAX_RESOURCES, open_result);
        (void)naos_handle_close(open_invocation);
        if (status != NA_STATUS_OK || open_result.actual_resources != 2)
        {
            smoke_log("smoke: open_pty_slave take failed\n");
            (void)naos_handle_close(master_handle);
            (void)naos_handle_close(manager);
            return;
        }
        const na_handle_t slave_handle = slave_resources[open_response.slave.value];
        if (open_response.job_control.value < open_result.actual_resources)
            (void)naos_handle_close(slave_resources[open_response.job_control.value]);
        auto slave_client = naos::system::TerminalSlave::TerminalSlaveClient(transport.async(), slave_handle);
        smoke_log("smoke: pty master/slave opened\n");

        const std::uint8_t hello[] = {'h', 'e', 'l', 'l', 'o', '\n'};
        naos::system::TerminalMaster::write_request master_write{};
        master_write.size = sizeof(hello);
        master_write.data = {hello, sizeof(hello)};
        na_handle_t write_invocation = NA_HANDLE_INVALID;
        status = master_client.submit_write(master_write, nullptr, 0, &write_invocation, wire, sizeof(wire));
        if (status == NA_STATUS_OK)
            status = wait_invocation(write_invocation) == 0 ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        if (status == NA_STATUS_OK)
        {
            naos::system::TerminalMaster::write_response write_response{};
            na_result_frame_t write_result{};
            status = master_client.take_write(write_invocation, write_response, wire, sizeof(wire), nullptr, 0,
                                              write_result);
        }
        (void)naos_handle_close(write_invocation);
        if (status != NA_STATUS_OK)
        {
            smoke_log("smoke: master write failed\n");
            (void)naos_handle_close(master_handle);
            (void)naos_handle_close(slave_handle);
            (void)naos_handle_close(manager);
            return;
        }

        naos::system::TerminalSlave::read_request slave_read{};
        slave_read.size = sizeof(hello);
        slave_read.flags = 0;
        na_handle_t read_invocation = NA_HANDLE_INVALID;
        status = slave_client.submit_read(slave_read, nullptr, 0, &read_invocation, wire, sizeof(wire));
        if (status == NA_STATUS_OK)
            status = wait_invocation(read_invocation) == 0 ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        std::uint8_t slave_data[64]{};
        std::size_t slave_data_size = 0;
        if (status == NA_STATUS_OK)
        {
            naos::system::TerminalSlave::read_response read_response{};
            na_result_frame_t read_result{};
            status =
                slave_client.take_read(read_invocation, read_response, wire, sizeof(wire), nullptr, 0, read_result);
            if (status == NA_STATUS_OK && read_response.data.size <= sizeof(slave_data))
            {
                memcpy(slave_data, read_response.data.data, read_response.data.size);
                slave_data_size = read_response.data.size;
            }
        }
        (void)naos_handle_close(read_invocation);
        if (status == NA_STATUS_OK && slave_data_size == sizeof(hello) && memcmp(slave_data, hello, sizeof(hello)) == 0)
        {
            smoke_log("smoke: pty master->slave data ok\n");
        }
        else
        {
            smoke_log("smoke: pty master->slave data mismatch\n");
        }

        const std::uint8_t output[] = {'o', 'k'};
        naos::system::TerminalSlave::write_request slave_write{};
        slave_write.size = sizeof(output);
        slave_write.data = {output, sizeof(output)};
        na_handle_t slave_write_invocation = NA_HANDLE_INVALID;
        status = slave_client.submit_write(slave_write, nullptr, 0, &slave_write_invocation, wire, sizeof(wire));
        if (status == NA_STATUS_OK)
            status = wait_invocation(slave_write_invocation) == 0 ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        (void)naos_handle_close(slave_write_invocation);

        naos::system::TerminalMaster::read_request master_read{};
        master_read.size = sizeof(output);
        na_handle_t master_read_invocation = NA_HANDLE_INVALID;
        status = master_client.submit_read(master_read, nullptr, 0, &master_read_invocation, wire, sizeof(wire));
        if (status == NA_STATUS_OK)
            status = wait_invocation(master_read_invocation) == 0 ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        std::uint8_t master_data[64]{};
        std::size_t master_data_size = 0;
        if (status == NA_STATUS_OK)
        {
            naos::system::TerminalMaster::read_response read_response{};
            na_result_frame_t read_result{};
            status = master_client.take_read(master_read_invocation, read_response, wire, sizeof(wire), nullptr, 0,
                                             read_result);
            if (status == NA_STATUS_OK && read_response.data.size <= sizeof(master_data))
            {
                memcpy(master_data, read_response.data.data, read_response.data.size);
                master_data_size = read_response.data.size;
            }
        }
        (void)naos_handle_close(master_read_invocation);
        if (status == NA_STATUS_OK && master_data_size == sizeof(output) &&
            memcmp(master_data, output, sizeof(output)) == 0)
        {
            smoke_log("smoke: pty slave->master data ok\n");
        }
        else
        {
            smoke_log("smoke: pty slave->master data mismatch\n");
        }

        (void)naos_handle_close(master_handle);
        (void)naos_handle_close(slave_handle);
    }
    else
    {
        smoke_printf("smoke: ttyd create_pty failed status=%d outcome=%d reason=%d res=%llu error=%lld\n",
                     static_cast<int>(status), static_cast<int>(result.execution_outcome),
                     static_cast<int>(result.outcome_reason), static_cast<unsigned long long>(result.actual_resources),
                     static_cast<long long>(result.protocol_error));
        char message[160]{};
        snprintf(message, sizeof(message),
                 "smoke: create_pty status=%d outcome=%d reason=%d res=%llu error=%lld bytes=%llu number=%llu\n",
                 static_cast<int>(status), static_cast<int>(result.execution_outcome),
                 static_cast<int>(result.outcome_reason), static_cast<unsigned long long>(result.actual_resources),
                 static_cast<long long>(result.protocol_error), static_cast<unsigned long long>(result.actual_bytes),
                 static_cast<unsigned long long>(response.number));
        smoke_log(message);
    }
    smoke_log("smoke: ttyd smoke done\n");
    (void)naos_handle_close(manager);
}

void malformed_message_smoke()
{
    smoke_log("smoke: malformed message smoke begin\n");
    na_handle_t manager = NA_HANDLE_INVALID;
    if (naos_service_connect_versioned("naos://system/terminal", &naos::system::TerminalManager::protocol_uuid,
                                       NA_PROTOCOL_RIGHT_INVOKE, naos::system::TerminalManager::revision,
                                       naos::system::TerminalManager::features, &manager) != 0)
    {
        smoke_log("smoke: malformed smoke connect failed\n");
        return;
    }
    std::uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    na_submit_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.method_id = NA_METHOD_TERMINAL_MANAGER_CREATE_PTY;
    frame.request = reinterpret_cast<std::uint64_t>(garbage);
    frame.request_bytes = 3;
    na_handle_t invocation = NA_HANDLE_INVALID;
    const auto status = _na_invoke_submit(manager, &frame, &invocation);
    if (status == NA_STATUS_OK && wait_invocation(invocation) == 0)
    {
        na_result_frame_t result{};
        (void)_na_invocation_take_result(invocation, &result);
        (void)naos_handle_close(invocation);
    }
    (void)naos_handle_close(manager);

    const int probe = open("/dev/ptmx", O_RDWR);
    if (probe >= 0)
    {
        close(probe);
        smoke_log("smoke: malformed message isolation ok\n");
    }
    else
    {
        smoke_log("smoke: malformed message isolation failed\n");
    }
}

void tty_fuzz_smoke()
{
    smoke_log("smoke: tty fuzz smoke begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        smoke_log("smoke: fuzz ptmx failed\n");
        return;
    }
    int number = -1;
    int unlock = 0;
    if (ioctl(master, TIOCGPTN, &number) != 0 || grantpt(master) != 0 || ioctl(master, TIOCSPTLCK, &unlock) != 0)
    {
        close(master);
        return;
    }
    char path[32]{};
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    const int slave = open(path, O_RDWR);
    if (slave < 0)
    {
        close(master);
        return;
    }
    struct termios raw{};
    if (ioctl(master, TCGETS, &raw) == 0)
    {
        raw.c_iflag = 0;
        raw.c_oflag = 0;
        raw.c_cflag = CS8 | CREAD;
        raw.c_lflag = 0;
        (void)ioctl(master, TCSETS, &raw);
    }
    (void)fcntl(slave, F_SETFL, O_NONBLOCK);

    std::uint32_t seed = 0x12345678;
    bool ok = true;
    std::uint8_t chunk[64]{};
    for (int round = 0; round < 64 && ok; round++)
    {
        for (int i = 0; i < 64; i++)
        {
            seed = seed * 1664525u + 1013904223u;
            chunk[i] = static_cast<std::uint8_t>(seed >> 24);
        }
        const ssize_t written = write(master, chunk, sizeof(chunk));
        if (written != static_cast<ssize_t>(sizeof(chunk)))
            ok = false;
    }
    for (int i = 0; i < 64; i++)
    {
        std::uint8_t drain[128]{};
        const ssize_t n = read(slave, drain, sizeof(drain));
        if (n < 0 && errno == EAGAIN)
            break;
        if (n < 0)
        {
            ok = false;
            break;
        }
    }
    close(slave);
    close(master);
    smoke_log(ok ? "smoke: tty fuzz smoke ok\n" : "smoke: tty fuzz smoke failed\n");
}

void framebuffer_lifetime_smoke()
{
    smoke_log("smoke: framebuffer lifetime smoke begin\n");
    const int fd = open("/dev/fb0", O_RDWR | O_EXCL);
    if (fd < 0)
    {
        smoke_log("smoke: fb0 open failed\n");
        return;
    }
    fb_fix_screeninfo fix{};
    fb_var_screeninfo var{};
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0)
    {
        close(fd);
        return;
    }
    const std::size_t visible_frame_bytes = static_cast<std::size_t>(fix.line_length) * var.yres;
    if (fix.smem_len < visible_frame_bytes)
    {
        close(fd);
        smoke_log("smoke: fb0 memory is smaller than visible frame\n");
        return;
    }
    const std::size_t mapped_bytes = fix.smem_len;
    void *mapping = mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
    {
        close(fd);
        smoke_log("smoke: fb0 mmap failed\n");
        return;
    }
    close(fd);

    bool busy_before_unmap = false;
    const int second = open("/dev/fb0", O_RDWR | O_EXCL);
    if (second < 0)
    {
        busy_before_unmap = true;
    }
    else
    {
        close(second);
    }
    (void)munmap(mapping, mapped_bytes);

    const int third = open("/dev/fb0", O_RDWR | O_EXCL);
    const bool free_after_unmap = third >= 0;
    if (third >= 0)
        close(third);

    if (busy_before_unmap && free_after_unmap)
        smoke_log("smoke: framebuffer lifetime ok\n");
    else
        smoke_log("smoke: framebuffer lifetime failed\n");
}

bool wait_for_job_control_stop(pid_t child, int expected_signal, const char *signal_name)
{
    int child_status = 0;
    for (int attempt = 0; attempt < 100; attempt++)
    {
        const pid_t waited = waitpid(child, &child_status, WUNTRACED | WNOHANG);
        if (waited == child)
        {
            if (WIFSTOPPED(child_status) && WSTOPSIG(child_status) == expected_signal)
            {
                smoke_printf("smoke: %s stop ok\n", signal_name);
                return true;
            }
            smoke_printf("smoke: %s stop missing status=%d\n", signal_name, child_status);
            return false;
        }
        if (waited < 0)
        {
            if (errno == EINTR)
                continue;
            smoke_printf("smoke: %s wait failed errno=%d\n", signal_name, errno);
            return false;
        }
        (void)usleep(10'000);
    }
    smoke_printf("smoke: %s wait timed out\n", signal_name);
    return false;
}

void openpty_smoke()
{
    smoke_log("smoke: openpty begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        smoke_log("smoke: openpty master failed\n");
        return;
    }
    smoke_log("smoke: openpty master opened\n");
    int number = -1;
    if (ioctl(master, TIOCGPTN, &number) != 0)
    {
        smoke_log("smoke: TIOCGPTN failed\n");
        close(master);
        return;
    }
    int unlock = 0;
    if (grantpt(master) != 0 || ioctl(master, TIOCSPTLCK, &unlock) != 0)
    {
        smoke_log("smoke: TIOCSPTLCK failed\n");
        close(master);
        return;
    }
    char path[32]{};
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    const int slave = open(path, O_RDWR);
    if (slave < 0)
    {
        smoke_log("smoke: openpty slave failed\n");
        close(master);
        return;
    }
    smoke_log("smoke: openpty opened\n");

    smoke_log("smoke: job control begin\n");
    const pid_t session_id = setsid();
    if (session_id < 0)
    {
        smoke_printf("smoke: setsid failed errno=%d\n", errno);
        close(slave);
        close(master);
        return;
    }
    smoke_printf("smoke: job control session=%d\n", static_cast<int>(session_id));
    if (ioctl(slave, TIOCSCTTY, 0) != 0)
        smoke_log("smoke: TIOCSCTTY failed\n");
    else
        smoke_log("smoke: TIOCSCTTY ok\n");
    const pid_t self_pid = getpid();
    if (ioctl(master, TIOCSPGRP, &self_pid) != 0)
        smoke_log("smoke: TIOCSPGRP failed\n");
    else
        smoke_log("smoke: TIOCSPGRP ok\n");
    int pgrp = -1;
    if (ioctl(master, TIOCGPGRP, &pgrp) != 0)
        smoke_log("smoke: TIOCGPGRP failed\n");
    else if (pgrp == static_cast<int>(self_pid))
        smoke_log("smoke: TIOCGPGRP ok\n");
    else
        smoke_log("smoke: TIOCGPGRP mismatch\n");
    struct winsize smoke_ws{};
    smoke_ws.ws_row = 24;
    smoke_ws.ws_col = 80;
    if (ioctl(master, TIOCSWINSZ, &smoke_ws) != 0)
        smoke_log("smoke: TIOCSWINSZ failed\n");
    else
        smoke_log("smoke: TIOCSWINSZ ok\n");

    smoke_log("smoke: SIGTTIN smoke begin\n");
    const pid_t bg = fork();
    if (bg == 0)
    {
        if (setpgid(0, 0) != 0)
            _exit(120);
        char bg_char = 0;
        const ssize_t bg_n = read(slave, &bg_char, 1);
        _exit(bg_n >= 0 ? 121 : 122);
    }
    if (bg > 0)
    {
        if (setpgid(bg, bg) != 0)
            smoke_log("smoke: bg setpgid failed\n");
        (void)wait_for_job_control_stop(bg, SIGTTIN, "SIGTTIN");
        (void)kill(bg, SIGKILL);
        (void)waitpid(bg, nullptr, 0);
    }

    smoke_log("smoke: SIGTTOU smoke begin\n");
    struct termios bg_termios{};
    if (ioctl(master, TCGETS, &bg_termios) == 0)
    {
        bg_termios.c_lflag |= TOSTOP;
        if (ioctl(slave, TCSETS, &bg_termios) != 0)
            smoke_printf("smoke: TCSETS TOSTOP failed errno=%d\n", errno);
    }
    const pid_t bgw = fork();
    if (bgw == 0)
    {
        if (setpgid(0, 0) != 0)
            _exit(130);
        const char bgw_char = 'x';
        const ssize_t bgw_n = write(slave, &bgw_char, 1);
        if (bgw_n != 1)
        {
            char bgw_message[64]{};
            snprintf(bgw_message, sizeof(bgw_message), "smoke: bgw write errno=%d\n", errno);
            smoke_log(bgw_message);
        }
        _exit(bgw_n == 1 ? 131 : 132);
    }
    if (bgw > 0)
    {
        if (setpgid(bgw, bgw) != 0)
            smoke_log("smoke: bgw setpgid failed\n");
        (void)wait_for_job_control_stop(bgw, SIGTTOU, "SIGTTOU");
        (void)kill(bgw, SIGKILL);
        (void)waitpid(bgw, nullptr, 0);
    }
    bg_termios.c_lflag &= ~TOSTOP;
    (void)ioctl(master, TCSETS, &bg_termios);

    const char hello[] = "hi\n";
    const ssize_t written_hello = write(master, hello, sizeof(hello) - 1);
    if (written_hello != static_cast<ssize_t>(sizeof(hello) - 1))
    {
        smoke_log("smoke: openpty master write failed\n");
    }
    else
    {
        char buffer[16]{};
        const ssize_t n = read(slave, buffer, sizeof(buffer));
        if (n == static_cast<ssize_t>(sizeof(hello) - 1) && memcmp(buffer, hello, sizeof(hello) - 1) == 0)
            smoke_log("smoke: openpty master->slave ok\n");
        else
            smoke_log("smoke: openpty master->slave mismatch\n");
    }

    const char out[] = "ok";
    const ssize_t written_out = write(slave, out, sizeof(out) - 1);
    if (written_out != static_cast<ssize_t>(sizeof(out) - 1))
    {
        smoke_log("smoke: openpty slave write failed\n");
    }
    else
    {
        char buffer[16]{};
        const ssize_t n = read(master, buffer, sizeof(buffer));
        if (n >= static_cast<ssize_t>(sizeof(out) - 1) &&
            memcmp(buffer + n - (sizeof(out) - 1), out, sizeof(out) - 1) == 0)
            smoke_log("smoke: openpty slave->master ok\n");
        else
            smoke_log("smoke: openpty slave->master mismatch\n");
    }

    const int duplicate = dup(master);
    if (duplicate < 0)
    {
        smoke_log("smoke: openpty dup failed\n");
    }
    else
    {
        close(duplicate);
    }

    const pid_t child = fork();
    if (child == 0)
    {
        const char child_data[] = "c\n";
        (void)write(master, child_data, sizeof(child_data) - 1);
        _exit(0);
    }
    if (child > 0)
    {
        int child_status = 0;
        (void)waitpid(child, &child_status, 0);
        char buffer[16]{};
        const ssize_t n = read(slave, buffer, sizeof(buffer));
        if (n == 2 && memcmp(buffer, "c\n", 2) == 0)
            smoke_log("smoke: openpty fork shared ok\n");
        else
            smoke_log("smoke: openpty fork shared mismatch\n");
    }

    if (ioctl(slave, TIOCNOTTY) != 0)
        smoke_printf("smoke: TIOCNOTTY failed errno=%d\n", errno);
    close(slave);
    close(master);
    smoke_log("smoke: openpty closed\n");
}

void pty_shell_smoke()
{
    smoke_log("smoke: pty shell begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        smoke_log("smoke: pty shell master failed\n");
        return;
    }
    int number = -1;
    if (ioctl(master, TIOCGPTN, &number) != 0 || number < 0)
    {
        smoke_log("smoke: pty shell TIOCGPTN failed\n");
        close(master);
        return;
    }
    int unlock = 0;
    if (grantpt(master) != 0 || ioctl(master, TIOCSPTLCK, &unlock) != 0)
    {
        smoke_log("smoke: pty shell TIOCSPTLCK failed\n");
        close(master);
        return;
    }
    char path[32]{};
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    const int slave = open(path, O_RDWR);
    if (slave < 0)
    {
        smoke_log("smoke: pty shell slave failed\n");
        close(master);
        return;
    }
    smoke_log("smoke: pty shell slave opened\n");

    char *shell_argv[] = {const_cast<char *>("sh"), nullptr};
    pid_t pid = -1;
    const int spawn_error = naos_native_spawn_stdio(&pid, "/bin/busybox", shell_argv, environ, slave, slave, slave);
    if (spawn_error != 0 || pid <= 0)
    {
        smoke_log("smoke: pty shell spawn failed\n");
        close(slave);
        close(master);
        return;
    }
    smoke_log("smoke: pty shell forked\n");

    const char command[] = "echo PTY_SHELL_OK\n";
    (void)write(master, command, sizeof(command) - 1);
    char output[512]{};
    std::size_t output_size = 0;
    bool ok = false;
    for (int i = 0; i < 12 && !ok; i++)
    {
        char chunk[128]{};
        const ssize_t n = read(master, chunk, sizeof(chunk) - 1);
        if (n > 0)
        {
            if (output_size + static_cast<std::size_t>(n) < sizeof(output))
            {
                memcpy(output + output_size, chunk, static_cast<std::size_t>(n));
                output_size += static_cast<std::size_t>(n);
            }
            output[output_size] = 0;
            if (strstr(output, "PTY_SHELL_OK") != nullptr)
                ok = true;
        }
        sleep(1);
    }
    if (ok)
        smoke_log("smoke: pty shell output ok\n");
    else
        smoke_log("smoke: pty shell output missing\n");

    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, nullptr, 0);
    close(slave);
    close(master);
}

int run_smoke_suite()
{
    ttyd_smoke();
    malformed_message_smoke();
    tty_fuzz_smoke();
    openpty_smoke();
    framebuffer_lifetime_smoke();

    smoke_log("smoke: PTY limit stress begin\n");
    bool pty_stress_ok = true;
    for (int i = 0; i < 300; i++)
    {
        const int stress_master = open("/dev/ptmx", O_RDWR);
        if (stress_master < 0)
        {
            pty_stress_ok = false;
            break;
        }
        int stress_number = -1;
        int stress_unlock = 0;
        if (ioctl(stress_master, TIOCGPTN, &stress_number) != 0 || grantpt(stress_master) != 0 ||
            ioctl(stress_master, TIOCSPTLCK, &stress_unlock) != 0)
        {
            pty_stress_ok = false;
            close(stress_master);
            break;
        }
        close(stress_master);
    }
    smoke_log(pty_stress_ok ? "smoke: PTY limit stress ok\n" : "smoke: PTY limit stress failed\n");
    pty_shell_smoke();
    smoke_log("smoke: ttyd kill recovery is supervisor-owned; skipped in standalone runner\n");
    const int tls_status = run_mlibc_tls_smoke();
    smoke_log(tls_status == 0 ? "smoke: mlibc TLS smoke ok\n" : "smoke: mlibc TLS smoke failed\n");
    return tls_status;
}

} // namespace

extern "C" int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--mlibc-tls") == 0)
    {
        const int tls_status = run_mlibc_tls_smoke();
        smoke_log(tls_status == 0 ? "smoke: mlibc TLS smoke ok\n" : "smoke: mlibc TLS smoke failed\n");
        return tls_status;
    }
    return run_smoke_suite();
}
