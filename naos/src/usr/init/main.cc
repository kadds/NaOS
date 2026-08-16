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

namespace
{
int g_ttyd_pid = -1;
na_handle_t g_terminal_factory = NA_HANDLE_INVALID;
na_handle_t g_console_frontend = NA_HANDLE_INVALID;
na_handle_t g_input_event_source = NA_HANDLE_INVALID;

bool ensure_terminal_factory()
{
    if (g_terminal_factory != NA_HANDLE_INVALID)
        return true;
    if (naos_take_terminal_driver_factory(&g_terminal_factory) != 0 || g_terminal_factory == NA_HANDLE_INVALID)
    {
        _s_log("init: terminal factory resolve failed\n");
        g_terminal_factory = NA_HANDLE_INVALID;
        return false;
    }
    return true;
}

bool ensure_input_event_source()
{
    if (g_input_event_source != NA_HANDLE_INVALID)
        return true;
    if (naos_take_input_event_source(&g_input_event_source) != 0 || g_input_event_source == NA_HANDLE_INVALID)
    {
        _s_log("init: input event source capability missing\n");
        g_input_event_source = NA_HANDLE_INVALID;
        return false;
    }
    return true;
}

bool ensure_console_frontend()
{
    if (g_console_frontend != NA_HANDLE_INVALID)
        return true;
    if (naos_take_console_frontend(&g_console_frontend) != 0 || g_console_frontend == NA_HANDLE_INVALID)
    {
        _s_log("init: console frontend capability missing\n");
        g_console_frontend = NA_HANDLE_INVALID;
        return false;
    }
    return true;
}

bool spawn_ttyd_process(int *pid)
{
    if (pid == nullptr || !ensure_terminal_factory())
        return false;

    const na_handle_t factory_for_child = g_terminal_factory;
    g_terminal_factory = NA_HANDLE_INVALID;

    char *ttyd_argv[] = {const_cast<char *>("ttyd"), nullptr};
    const int spawn_error = naos_native_spawn_with_terminal_factory_and_service_manager(pid, "/bin/ttyd", ttyd_argv,
                                                                                        environ, factory_for_child);
    if (spawn_error != 0 || *pid <= 0)
    {
        _s_log("init: ttyd spawn failed\n");
        return false;
    }
    return true;
}

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
    printf("init: spawning ttyd...\n");
    _s_log("init: spawning ttyd...\n");
    int ttyd_pid = -1;
    if (!spawn_ttyd_process(&ttyd_pid))
    {
        printf("init: spawn ttyd failed\n");
        _s_log("init: spawn ttyd failed\n");
        return;
    }
    _s_log("init: ttyd spawned\n");
    g_ttyd_pid = ttyd_pid;
    sleep(2);
    _s_log("init: ttyd sleep done\n");
    _s_log("init: connecting terminal manager...\n");

    na_handle_t manager = NA_HANDLE_INVALID;
    int error = naos_service_connect_versioned("naos://system/terminal", &naos::system::TerminalManager::protocol_uuid,
                                               NA_PROTOCOL_RIGHT_INVOKE, naos::system::TerminalManager::revision,
                                               naos::system::TerminalManager::features, &manager);
    if (error != 0)
    {
        printf("init: connect terminal manager failed %d\n", error);
        _s_log("init: connect terminal manager failed\n");
        return;
    }
    _s_log("init: connected terminal manager\n");

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
        printf("init: create_pty submit failed %d\n", static_cast<int>(status));
        _s_log("init: create_pty submit failed\n");
        (void)naos_handle_close(manager);
        return;
    }
    _s_log("init: create_pty submitted\n");
    if (wait_invocation(invocation) != 0)
    {
        printf("init: create_pty wait failed\n");
        _s_log("init: create_pty wait failed\n");
        (void)naos_handle_close(invocation);
        (void)naos_handle_close(manager);
        return;
    }
    _s_log("init: create_pty completed\n");
    naos::system::TerminalManager::create_pty_response response{};
    na_handle_t resources[NA_CHANNEL_MAX_RESOURCES]{};
    na_result_frame_t result{};
    status =
        client.take_create_pty(invocation, response, wire, sizeof(wire), resources, NA_CHANNEL_MAX_RESOURCES, result);
    (void)naos_handle_close(invocation);
    if (status == NA_STATUS_OK && result.execution_outcome == NA_EXECUTION_NONE && result.protocol_error == 0 &&
        result.actual_resources == 2)
    {
        printf("init: ttyd create_pty ok number=%llu\n", static_cast<unsigned long long>(response.number));
        _s_log("init: ttyd create_pty ok\n");

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
            _s_log("init: open_pty_slave failed\n");
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
            _s_log("init: open_pty_slave take failed\n");
            (void)naos_handle_close(master_handle);
            (void)naos_handle_close(manager);
            return;
        }
        const na_handle_t slave_handle = slave_resources[open_response.slave.value];
        if (open_response.job_control.value < open_result.actual_resources)
            (void)naos_handle_close(slave_resources[open_response.job_control.value]);
        auto slave_client = naos::system::TerminalSlave::TerminalSlaveClient(transport.async(), slave_handle);
        _s_log("init: pty master/slave opened\n");

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
            _s_log("init: master write failed\n");
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
            _s_log("init: pty master->slave data ok\n");
        }
        else
        {
            _s_log("init: pty master->slave data mismatch\n");
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
            _s_log("init: pty slave->master data ok\n");
        }
        else
        {
            _s_log("init: pty slave->master data mismatch\n");
        }

        (void)naos_handle_close(master_handle);
        (void)naos_handle_close(slave_handle);
    }
    else
    {
        printf("init: ttyd create_pty failed status=%d outcome=%d reason=%d res=%llu error=%lld\n",
               static_cast<int>(status), static_cast<int>(result.execution_outcome),
               static_cast<int>(result.outcome_reason), static_cast<unsigned long long>(result.actual_resources),
               static_cast<long long>(result.protocol_error));
        char message[160]{};
        snprintf(message, sizeof(message),
                 "init: create_pty status=%d outcome=%d reason=%d res=%llu error=%lld bytes=%llu number=%llu\n",
                 static_cast<int>(status), static_cast<int>(result.execution_outcome),
                 static_cast<int>(result.outcome_reason), static_cast<unsigned long long>(result.actual_resources),
                 static_cast<long long>(result.protocol_error), static_cast<unsigned long long>(result.actual_bytes),
                 static_cast<unsigned long long>(response.number));
        _s_log(message);
    }
    _s_log("init: ttyd smoke done\n");
    (void)naos_handle_close(manager);
}

void malformed_message_smoke()
{
    _s_log("init: malformed message smoke begin\n");
    na_handle_t manager = NA_HANDLE_INVALID;
    if (naos_service_connect_versioned("naos://system/terminal", &naos::system::TerminalManager::protocol_uuid,
                                       NA_PROTOCOL_RIGHT_INVOKE, naos::system::TerminalManager::revision,
                                       naos::system::TerminalManager::features, &manager) != 0)
    {
        _s_log("init: malformed smoke connect failed\n");
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
        _s_log("init: malformed message isolation ok\n");
    }
    else
    {
        _s_log("init: malformed message isolation failed\n");
    }
}

void tty_fuzz_smoke()
{
    _s_log("init: tty fuzz smoke begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        _s_log("init: fuzz ptmx failed\n");
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
    _s_log(ok ? "init: tty fuzz smoke ok\n" : "init: tty fuzz smoke failed\n");
}

void framebuffer_lifetime_smoke()
{
    _s_log("init: framebuffer lifetime smoke begin\n");
    const int fd = open("/dev/fb0", O_RDWR | O_EXCL);
    if (fd < 0)
    {
        _s_log("init: fb0 open failed\n");
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
        _s_log("init: fb0 memory is smaller than visible frame\n");
        return;
    }
    const std::size_t mapped_bytes = fix.smem_len;
    void *mapping = mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
    {
        close(fd);
        _s_log("init: fb0 mmap failed\n");
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
        _s_log("init: framebuffer lifetime ok\n");
    else
        _s_log("init: framebuffer lifetime failed\n");
}

void openpty_smoke()
{
    _s_log("init: openpty begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        _s_log("init: openpty master failed\n");
        return;
    }
    _s_log("init: openpty master opened\n");
    int number = -1;
    if (ioctl(master, TIOCGPTN, &number) != 0)
    {
        _s_log("init: TIOCGPTN failed\n");
        close(master);
        return;
    }
    int unlock = 0;
    if (grantpt(master) != 0 || ioctl(master, TIOCSPTLCK, &unlock) != 0)
    {
        _s_log("init: TIOCSPTLCK failed\n");
        close(master);
        return;
    }
    char path[32]{};
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    const int slave = open(path, O_RDWR);
    if (slave < 0)
    {
        _s_log("init: openpty slave failed\n");
        close(master);
        return;
    }
    _s_log("init: openpty opened\n");

    _s_log("init: job control begin\n");
    int ctty_force = 1;
    if (ioctl(slave, TIOCSCTTY, &ctty_force) != 0)
        _s_log("init: TIOCSCTTY failed\n");
    else
        _s_log("init: TIOCSCTTY ok\n");
    const pid_t self_pid = getpid();
    if (ioctl(master, TIOCSPGRP, &self_pid) != 0)
        _s_log("init: TIOCSPGRP failed\n");
    else
        _s_log("init: TIOCSPGRP ok\n");
    int pgrp = -1;
    if (ioctl(master, TIOCGPGRP, &pgrp) != 0)
        _s_log("init: TIOCGPGRP failed\n");
    else if (pgrp == static_cast<int>(self_pid))
        _s_log("init: TIOCGPGRP ok\n");
    else
        _s_log("init: TIOCGPGRP mismatch\n");
    struct winsize smoke_ws{};
    smoke_ws.ws_row = 24;
    smoke_ws.ws_col = 80;
    if (ioctl(master, TIOCSWINSZ, &smoke_ws) != 0)
        _s_log("init: TIOCSWINSZ failed\n");
    else
        _s_log("init: TIOCSWINSZ ok\n");

    _s_log("init: SIGTTIN smoke begin\n");
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
            _s_log("init: bg setpgid failed\n");
        int bg_status = 0;
        if (waitpid(bg, &bg_status, WUNTRACED) < 0)
            _s_log("init: bg wait failed\n");
        else if (WIFSTOPPED(bg_status) && WSTOPSIG(bg_status) == SIGTTIN)
            _s_log("init: SIGTTIN stop ok\n");
        else
            _s_log("init: SIGTTIN missing\n");
        (void)kill(bg, SIGKILL);
        (void)waitpid(bg, nullptr, 0);
    }

    _s_log("init: SIGTTOU smoke begin\n");
    struct termios bg_termios{};
    if (ioctl(master, TCGETS, &bg_termios) == 0)
    {
        bg_termios.c_lflag |= TOSTOP;
        (void)ioctl(master, TCSETS, &bg_termios);
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
            snprintf(bgw_message, sizeof(bgw_message), "init: bgw write errno=%d\n", errno);
            _s_log(bgw_message);
        }
        _exit(bgw_n == 1 ? 131 : 132);
    }
    if (bgw > 0)
    {
        if (setpgid(bgw, bgw) != 0)
            _s_log("init: bgw setpgid failed\n");
        int bgw_status = 0;
        if (waitpid(bgw, &bgw_status, WUNTRACED) < 0)
            _s_log("init: bgw wait failed\n");
        else if (WIFSTOPPED(bgw_status) && WSTOPSIG(bgw_status) == SIGTTOU)
            _s_log("init: SIGTTOU stop ok\n");
        else
            _s_log("init: SIGTTOU missing\n");
        (void)kill(bgw, SIGKILL);
        (void)waitpid(bgw, nullptr, 0);
    }
    bg_termios.c_lflag &= ~TOSTOP;
    (void)ioctl(master, TCSETS, &bg_termios);

    const char hello[] = "hi\n";
    const ssize_t written_hello = write(master, hello, sizeof(hello) - 1);
    if (written_hello != static_cast<ssize_t>(sizeof(hello) - 1))
    {
        _s_log("init: openpty master write failed\n");
    }
    else
    {
        char buffer[16]{};
        const ssize_t n = read(slave, buffer, sizeof(buffer));
        if (n == static_cast<ssize_t>(sizeof(hello) - 1) && memcmp(buffer, hello, sizeof(hello) - 1) == 0)
            _s_log("init: openpty master->slave ok\n");
        else
            _s_log("init: openpty master->slave mismatch\n");
    }

    const char out[] = "ok";
    const ssize_t written_out = write(slave, out, sizeof(out) - 1);
    if (written_out != static_cast<ssize_t>(sizeof(out) - 1))
    {
        _s_log("init: openpty slave write failed\n");
    }
    else
    {
        char buffer[16]{};
        const ssize_t n = read(master, buffer, sizeof(buffer));
        if (n >= static_cast<ssize_t>(sizeof(out) - 1) &&
            memcmp(buffer + n - (sizeof(out) - 1), out, sizeof(out) - 1) == 0)
            _s_log("init: openpty slave->master ok\n");
        else
            _s_log("init: openpty slave->master mismatch\n");
    }

    const int duplicate = dup(master);
    if (duplicate < 0)
    {
        _s_log("init: openpty dup failed\n");
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
            _s_log("init: openpty fork shared ok\n");
        else
            _s_log("init: openpty fork shared mismatch\n");
    }

    close(slave);
    close(master);
    _s_log("init: openpty closed\n");
}

void pty_shell_smoke()
{
    _s_log("init: pty shell begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        _s_log("init: pty shell master failed\n");
        return;
    }
    int number = -1;
    if (ioctl(master, TIOCGPTN, &number) != 0 || number < 0)
    {
        _s_log("init: pty shell TIOCGPTN failed\n");
        close(master);
        return;
    }
    int unlock = 0;
    if (grantpt(master) != 0 || ioctl(master, TIOCSPTLCK, &unlock) != 0)
    {
        _s_log("init: pty shell TIOCSPTLCK failed\n");
        close(master);
        return;
    }
    char path[32]{};
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    const int slave = open(path, O_RDWR);
    if (slave < 0)
    {
        _s_log("init: pty shell slave failed\n");
        close(master);
        return;
    }
    _s_log("init: pty shell slave opened\n");

    char *shell_argv[] = {const_cast<char *>("sh"), nullptr};
    pid_t pid = -1;
    const int spawn_error = naos_native_spawn_stdio(&pid, "/bin/busybox", shell_argv, environ, slave, slave, slave);
    if (spawn_error != 0 || pid <= 0)
    {
        _s_log("init: pty shell spawn failed\n");
        close(slave);
        close(master);
        return;
    }
    _s_log("init: pty shell forked\n");

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
        _s_log("init: pty shell output ok\n");
    else
        _s_log("init: pty shell output missing\n");

    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, nullptr, 0);
    close(slave);
    close(master);
}

void ttyd_kill_recovery_smoke()
{
    _s_log("init: ttyd kill recovery begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        _s_log("init: kill smoke ptmx failed\n");
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

    const pid_t reader = fork();
    if (reader == 0)
    {
        char reader_char = 0;
        const ssize_t reader_n = read(slave, &reader_char, 1);
        _exit(reader_n < 0 ? 140 : 141);
    }
    sleep(1);
    if (g_ttyd_pid > 0)
    {
        (void)kill(g_ttyd_pid, SIGKILL);
        int ttyd_status = 0;
        (void)waitpid(g_ttyd_pid, &ttyd_status, 0);
        _s_log("init: ttyd killed\n");
    }

    int reader_status = 0;
    bool reader_returned = false;
    for (int i = 0; i < 50 && !reader_returned; i++)
    {
        const pid_t waited = waitpid(reader, &reader_status, WNOHANG);
        if (waited == reader)
            reader_returned = true;
        else
            sleep(1);
    }
    if (reader_returned && WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 140)
        _s_log("init: pending read returned after ttyd kill ok\n");
    else
        _s_log("init: pending read did not return\n");
    close(slave);
    close(master);

    int new_pid = -1;
    if (spawn_ttyd_process(&new_pid))
    {
        g_ttyd_pid = new_pid;
        sleep(2);
        bool restart_ok = false;
        for (int i = 0; i < 4 && !restart_ok; i++)
        {
            const int probe = open("/dev/ptmx", O_RDWR);
            if (probe >= 0)
            {
                close(probe);
                restart_ok = true;
            }
            else
            {
                sleep(1);
            }
        }
        _s_log(restart_ok ? "init: ttyd restart ok\n" : "init: ttyd restart failed\n");
    }
    else
    {
        _s_log("init: ttyd respawn failed\n");
    }
}

bool start_ttyd()
{
    int ttyd_pid = -1;
    if (!spawn_ttyd_process(&ttyd_pid))
    {
        _s_log("init: ttyd start failed\n");
        return false;
    }
    g_ttyd_pid = ttyd_pid;
    for (int attempt = 0; attempt < 10; attempt++)
    {
        na_handle_t manager = NA_HANDLE_INVALID;
        const int error = naos_service_connect_versioned(
            "naos://system/terminal", &naos::system::TerminalManager::protocol_uuid, NA_PROTOCOL_RIGHT_INVOKE,
            naos::system::TerminalManager::revision, naos::system::TerminalManager::features, &manager);
        if (error == 0)
        {
            (void)naos_handle_close(manager);
            _s_log("init: ttyd ready\n");
            return true;
        }
        sleep(1);
    }
    (void)kill(ttyd_pid, SIGTERM);
    int status = 0;
    (void)waitpid(ttyd_pid, &status, 0);
    g_ttyd_pid = -1;
    _s_log("init: ttyd readiness timed out\n");
    return false;
}

void run_smoke_suite()
{
    ttyd_smoke();
    malformed_message_smoke();
    tty_fuzz_smoke();
    openpty_smoke();
    framebuffer_lifetime_smoke();

    _s_log("init: PTY limit stress begin\n");
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
    _s_log(pty_stress_ok ? "init: PTY limit stress ok\n" : "init: PTY limit stress failed\n");
    pty_shell_smoke();
    ttyd_kill_recovery_smoke();
}

bool spawn_consoled_process(pid_t *pid)
{
    if (pid == nullptr)
        return false;

    char *console_argv[] = {const_cast<char *>("consoled"), nullptr};
    *pid = -1;
    if (!ensure_input_event_source() || !ensure_console_frontend())
        return false;
    na_handle_t writer_for_child = NA_HANDLE_INVALID;
    if (_na_handle_duplicate(g_console_frontend, 0, &writer_for_child) != NA_STATUS_OK)
        return false;
    na_handle_t input_for_child = NA_HANDLE_INVALID;
    if (_na_handle_duplicate(g_input_event_source, 0, &input_for_child) != NA_STATUS_OK)
    {
        (void)_na_handle_close(writer_for_child);
        return false;
    }
    const na_bootstrap_capability_t capabilities[] = {
        {NA_BOOTSTRAP_CAPABILITY_CONSOLE_FRONTEND, writer_for_child},
        {NA_BOOTSTRAP_CAPABILITY_INPUT_EVENT_SOURCE, input_for_child},
    };
    const int spawn_error = naos_native_spawn_with_capabilities(
        pid, "/bin/consoled", console_argv, environ, capabilities, sizeof(capabilities) / sizeof(capabilities[0]));
    if (spawn_error != 0 || *pid <= 0)
    {
        _s_log("init: consoled spawn failed\n");
        *pid = -1;
        return false;
    }
    _s_log("init: consoled started\n");
    return true;
}

bool framebuffer_available()
{
    const int fd = open("/dev/fb0", O_RDWR | O_EXCL);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

bool spawn_user_shell(pid_t *shell, int *slave)
{
    if (shell == nullptr || slave == nullptr)
        return false;

    const int new_slave = open("/dev/console", O_RDWR);
    if (new_slave < 0)
    {
        _s_log("init: user console slave failed\n");
        return false;
    }

    char *shell_argv[] = {const_cast<char *>("sh"), const_cast<char *>("-i"), nullptr};
    *shell = -1;
    na_handle_t shell_process = NA_HANDLE_INVALID;
    const int shell_spawn = naos_native_spawn_stdio_deferred(shell, &shell_process, "/bin/busybox", shell_argv, environ,
                                                             new_slave, new_slave, new_slave);
    if (shell_spawn != 0 || *shell <= 0)
    {
        _s_log("init: user shell spawn failed\n");
        close(new_slave);
        *shell = -1;
        return false;
    }

    if (ioctl(new_slave, TIOCSCTTY, 0) != 0)
    {
        char error_message[80]{};
        snprintf(error_message, sizeof(error_message), "init: user shell TIOCSCTTY failed %d\n", errno);
        _s_log(error_message);
    }
    else
        _s_log("init: user shell TIOCSCTTY ok\n");
    if (setpgid(*shell, *shell) != 0)
        _s_log("init: user shell setpgid failed\n");
    else
        _s_log("init: user shell setpgid ok\n");
    int shell_pgid = static_cast<int>(*shell);
    if (ioctl(new_slave, TIOCSPGRP, &shell_pgid) != 0)
    {
        char error_message[80]{};
        snprintf(error_message, sizeof(error_message), "init: user shell TIOCSPGRP failed %d\n", errno);
        _s_log(error_message);
    }
    else
        _s_log("init: user shell TIOCSPGRP ok\n");
    int foreground_pgid = -1;
    if (ioctl(new_slave, TIOCGPGRP, &foreground_pgid) != 0)
    {
        char error_message[80]{};
        snprintf(error_message, sizeof(error_message), "init: user shell TIOCGPGRP failed %d\n", errno);
        _s_log(error_message);
    }
    else if (foreground_pgid == shell_pgid)
        _s_log("init: user shell TIOCGPGRP ok\n");
    else
        _s_log("init: user shell TIOCGPGRP mismatch\n");
    const int shell_start = naos_native_start_process(shell_process);
    if (shell_start != 0)
    {
        char start_error[64]{};
        snprintf(start_error, sizeof(start_error), "init: user shell start failed %d\n", shell_start);
        _s_log(start_error);
        (void)_na_handle_close(shell_process);
        close(new_slave);
        *shell = -1;
        return false;
    }
    (void)_na_handle_close(shell_process);

    *slave = new_slave;
    _s_log("init: user shell started\n");
    return true;
}

bool reap_process(pid_t *pid, const char *name)
{
    if (pid == nullptr || *pid <= 0)
        return false;

    int status = 0;
    const pid_t waited = waitpid(*pid, &status, WNOHANG);
    if (waited == 0 || (waited < 0 && errno == EINTR))
        return false;

    char message[96]{};
    if (waited == *pid)
    {
        if (WIFEXITED(status))
            snprintf(message, sizeof(message), "init: %s exited status=%d\n", name, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            snprintf(message, sizeof(message), "init: %s exited signal=%d\n", name, WTERMSIG(status));
        else
            snprintf(message, sizeof(message), "init: %s exited\n", name);
    }
    else
    {
        snprintf(message, sizeof(message), "init: %s wait failed errno=%d\n", name, errno);
    }
    _s_log(message);
    *pid = -1;
    return true;
}

void run_user_shell()
{
    pid_t console = -1;
    pid_t shell = -1;
    int slave = -1;

    _s_log("init: frontend supervision started\n");
    for (;;)
    {
        if (g_ttyd_pid <= 0)
        {
            _s_log("init: ttyd unavailable; restarting frontend\n");
            if (console > 0)
            {
                (void)kill(console, SIGTERM);
                (void)waitpid(console, nullptr, 0);
                console = -1;
            }
            if (shell > 0)
            {
                (void)kill(shell, SIGTERM);
                (void)waitpid(shell, nullptr, 0);
                shell = -1;
            }
            if (slave >= 0)
            {
                close(slave);
                slave = -1;
            }
            if (!start_ttyd())
            {
                sleep(5);
                continue;
            }
        }

        if (g_ttyd_pid > 0 && reap_process(&g_ttyd_pid, "ttyd"))
        {
            // The next iteration performs the ordered teardown and restart:
            // ttyd -> consoled -> shell, so no child keeps a stale endpoint.
            continue;
        }
        if (reap_process(&console, "consoled"))
        {
            // The shell's console fd points into consoled's frontend. Drop
            // that fd and restart the shell together with the frontend.
            if (shell > 0)
            {
                (void)kill(shell, SIGTERM);
                (void)waitpid(shell, nullptr, 0);
                shell = -1;
            }
            if (slave >= 0)
            {
                close(slave);
                slave = -1;
            }
            _s_log("init: consoled restart scheduled\n");
        }
        if (reap_process(&shell, "user shell"))
        {
            close(slave);
            slave = -1;
            _s_log("init: user shell restart scheduled\n");
        }
        if (console <= 0)
        {
            if (!ensure_console_frontend() || !framebuffer_available())
            {
                // Kernel terminal mode owns the scanout. Wait for the F1
                // enable transition before starting a new framebuffer frontend.
                sleep(1);
                continue;
            }
        }

        if (console <= 0 && !spawn_consoled_process(&console))
        {
            sleep(5);
            continue;
        }
        if (console > 0 && shell <= 0)
        {
            // A newly spawned frontend needs to claim the console master
            // before the first slave reader starts, otherwise ash sees an
            // initial EOF and exits cleanly.
            sleep(2);
        }

        if (console > 0 && shell <= 0)
            _s_log("init: spawning user shell\n");

        if (shell <= 0 && !spawn_user_shell(&shell, &slave))
        {
            sleep(5);
            continue;
        }

        sleep(1);
    }
}
} // namespace

extern "C" void main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--smoke") == 0)
    {
        run_smoke_suite();
        return;
    }

    if (!start_ttyd())
        return;
    run_user_shell();
}
