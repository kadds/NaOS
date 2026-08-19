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

bool run_optional_init_script()
{
    if (access("/etc/init.sh", R_OK) != 0 || access("/bin/sh", X_OK) != 0)
        return true;

    char *argv[] = {const_cast<char *>("sh"), const_cast<char *>("/etc/init.sh"), nullptr};
    pid_t pid = -1;
    if (naos_native_spawn_stdio(&pid, "/bin/busybox", argv, environ, STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO) != 0 ||
        pid <= 0)
    {
        _s_log("init: /etc/init.sh spawn failed\n");
        return false;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        _s_log("init: /etc/init.sh failed\n");
        return false;
    }
    _s_log("init: /etc/init.sh completed\n");
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
    if (!start_ttyd())
        return;
    (void)run_optional_init_script();
    run_user_shell();
}
