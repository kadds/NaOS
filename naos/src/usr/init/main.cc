#include <abi-bits/ioctls.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

[[gnu::weak]] void *__dso_handle;
extern char **environ;

extern "C" {
int ioctl(int fd, unsigned long request, ...);
int naos_native_spawn(pid_t *pid, const char *path, char *const argv[], char *const envp[]);
int naos_native_spawn_stdio(pid_t *pid, const char *path, char *const argv[], char *const envp[], int stdin_fd,
                            int stdout_fd, int stderr_fd);
int naos_native_spawn_stdio_deferred(pid_t *pid, na_handle_t *process, const char *path, char *const argv[],
                                     char *const envp[], int stdin_fd, int stdout_fd, int stderr_fd);
int naos_native_start_process(na_handle_t process);
int naos_native_spawn_stdio_with_service_manager(pid_t *pid, const char *path, char *const argv[], char *const envp[]);
int naos_native_install_root_namespace();
}

namespace
{
int g_ttyd_pid = -1;

bool spawn_ttyd_process(int *pid)
{
    if (pid == nullptr)
        return false;

    char *ttyd_argv[] = {const_cast<char *>("ttyd"), nullptr};
    printf("init: spawning ttyd\n");
    const int spawn_error = naos_native_spawn_stdio_with_service_manager(pid, "/bin/ttyd", ttyd_argv, environ);
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: ttyd spawn returned error=%d pid=%d errno=%d\n", spawn_error, *pid,
                 errno);
        printf("%s", message);
    }
    if (spawn_error != 0 || *pid <= 0)
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: ttyd spawn failed error=%d pid=%d errno=%d\n", spawn_error,
                 pid != nullptr ? *pid : -1, errno);
        printf("%s", message);
        return false;
    }
    return true;
}

bool start_ttyd()
{
    int ttyd_pid = -1;
    if (!spawn_ttyd_process(&ttyd_pid))
    {
        printf("init: ttyd start failed\n");
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
            // Manager registration alone does not guarantee that the first
            // `/dev/console` open has been materialized by ttyd.  Probe the
            // terminal endpoint through the normal libc path before handing
            // it to the shell, otherwise the process bootstrap can race the
            // manager's first PTY creation and observe EPIPE.
            // ttyd publishes its listener before its terminal factory answers,
            // so this is a readiness probe, not a fixed wait.  Poll it on a
            // short interval: a 1s step costs up to a full second of boot for
            // nothing once the factory is up, and the probe itself is a cheap
            // open plus tcgetattr.
            for (int console_attempt = 0; console_attempt < 400; console_attempt++)
            {
                const int console = open("/dev/console", O_RDWR);
                if (console >= 0)
                {
                    struct termios attributes{};
                    const bool ready = tcgetattr(console, &attributes) == 0;
                    close(console);
                    if (ready)
                    {
                        printf("init: ttyd console ready\n");
                        printf("init: ttyd ready\n");
                        return true;
                    }
                }
                usleep(5'000);
            }
            printf("init: ttyd console readiness timed out\n");
            return false;
        }
        sleep(1);
    }
    (void)kill(ttyd_pid, SIGTERM);
    int status = 0;
    (void)waitpid(ttyd_pid, &status, 0);
    g_ttyd_pid = -1;
    printf("init: ttyd readiness timed out\n");
    return false;
}

bool spawn_consoled_process(pid_t *pid)
{
    if (pid == nullptr)
        return false;

    char *console_argv[] = {const_cast<char *>("consoled"), nullptr};
    *pid = -1;
    const int spawn_error = naos_native_spawn_stdio(pid, "/bin/consoled", console_argv, environ, STDIN_FILENO,
                                                    STDOUT_FILENO, STDERR_FILENO);
    if (spawn_error != 0 || *pid <= 0)
    {
        printf("init: consoled spawn failed\n");
        *pid = -1;
        return false;
    }
    printf("init: consoled started\n");
    return true;
}

bool framebuffer_available()
{
    na_handle_t framebuffer = NA_HANDLE_INVALID;
    const int error = naos_service_resolve(NAOS_SERVICE_FRAMEBUFFER, &framebuffer);
    if (error != 0 || framebuffer == NA_HANDLE_INVALID)
    {
        char message[112]{};
        snprintf(message, sizeof(message), "init: framebuffer service resolve failed error=%u\n",
                 static_cast<unsigned>(error));
        printf("%s", message);
        return false;
    }
    (void)naos_handle_close(framebuffer);
    return true;
}

bool spawn_user_shell(pid_t *shell, int *slave)
{
    if (shell == nullptr || slave == nullptr)
        return false;

    const int new_slave = open("/dev/console", O_RDWR);
    if (new_slave < 0)
    {
        printf("init: user console slave failed\n");
        return false;
    }
    char *shell_argv[] = {const_cast<char *>("sh"), const_cast<char *>("-i"), nullptr};
    *shell = -1;
    na_handle_t shell_process = NA_HANDLE_INVALID;
    const int shell_spawn = naos_native_spawn_stdio_deferred(shell, &shell_process, "/bin/busybox", shell_argv, environ,
                                                             new_slave, new_slave, new_slave);
    if (shell_spawn != 0 || *shell <= 0)
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: user shell spawn failed error=%d pid=%d errno=%d\n", shell_spawn,
                 shell != nullptr ? *shell : -1, errno);
        printf("%s", message);
        close(new_slave);
        *shell = -1;
        return false;
    }

    if (ioctl(new_slave, TIOCSCTTY, 0) != 0)
    {
        char error_message[80]{};
        snprintf(error_message, sizeof(error_message), "init: user shell TIOCSCTTY failed %d\n", errno);
        printf("%s", error_message);
    }
    else
        printf("init: user shell TIOCSCTTY ok\n");
    if (setpgid(*shell, *shell) != 0)
        printf("init: user shell setpgid failed\n");
    else
        printf("init: user shell setpgid ok\n");
    int shell_pgid = static_cast<int>(*shell);
    if (ioctl(new_slave, TIOCSPGRP, &shell_pgid) != 0)
    {
        char error_message[80]{};
        snprintf(error_message, sizeof(error_message), "init: user shell TIOCSPGRP failed %d\n", errno);
        printf("%s", error_message);
    }
    else
        printf("init: user shell TIOCSPGRP ok\n");
    int foreground_pgid = -1;
    if (ioctl(new_slave, TIOCGPGRP, &foreground_pgid) != 0)
    {
        char error_message[80]{};
        snprintf(error_message, sizeof(error_message), "init: user shell TIOCGPGRP failed %d\n", errno);
        printf("%s", error_message);
    }
    else if (foreground_pgid == shell_pgid)
        printf("init: user shell TIOCGPGRP ok\n");
    else
        printf("init: user shell TIOCGPGRP mismatch\n");
    const int shell_start = naos_native_start_process(shell_process);
    if (shell_start != 0)
    {
        char start_error[64]{};
        snprintf(start_error, sizeof(start_error), "init: user shell start failed %d\n", shell_start);
        printf("%s", start_error);
        (void)_na_handle_close(shell_process);
        close(new_slave);
        *shell = -1;
        return false;
    }
    (void)_na_handle_close(shell_process);

    *slave = new_slave;
    printf("init: user shell started\n");
    return true;
}

bool reap_process(pid_t *pid, const char *name)
{
    if (pid == nullptr || *pid <= 0)
        return false;

    int status = 0;
    const pid_t waited = waitpid(*pid, &status, WNOHANG);
    char message[96]{};
    if (waited == 0 || (waited < 0 && errno == EINTR))
        return false;

    if (waited < 0)
    {
        const int wait_error = errno;
        errno = 0;
        const int probe = kill(*pid, 0);
        const int probe_error = errno;
        if (probe == 0 || probe_error != ESRCH)
        {
            snprintf(message, sizeof(message), "init: %s wait failed errno=%d; process still alive\n", name,
                     wait_error);
            printf("%s", message);
            return false;
        }

        snprintf(message, sizeof(message), "init: %s disappeared after wait error=%d\n", name, wait_error);
        printf("%s", message);
        *pid = -1;
        return true;
    }

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
    printf("%s", message);
    *pid = -1;
    return true;
}

/// Whether a hook script would actually do anything.
///
/// The production image ships an intentionally empty `/etc/init.sh` (a shebang,
/// comments, and an `exit 0`).  Running it means materialising a whole BusyBox
/// and paying a process round trip for nothing on every boot, so the script is
/// inspected first: only a real command justifies starting an interpreter.
bool script_has_work(const char *script)
{
    for (const char *line = script; *line != 0;)
    {
        const char *end = line;
        while (*end != 0 && *end != '\n')
            end++;
        const char *cursor = line;
        while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            cursor++;
        // A blank line, a comment, or a shebang carries no work.
        if (cursor < end && *cursor != '#')
        {
            // `exit` (with or without a status) only ends the script, so a
            // script whose sole command is an exit is still a no-op.
            constexpr size_t exit_len = 4;
            const bool is_exit = static_cast<size_t>(end - cursor) >= exit_len &&
                                 memcmp(cursor, "exit", exit_len) == 0 &&
                                 (static_cast<size_t>(end - cursor) == exit_len || cursor[exit_len] == ' ' ||
                                  cursor[exit_len] == '\t');
            if (!is_exit)
                return true;
        }
        line = *end == '\n' ? end + 1 : end;
    }
    return false;
}

bool run_optional_init_script()
{
    printf("init: checking optional init hook\n");
    const int init_script_access = access("/etc/init.sh", R_OK);
    const int shell_access = access("/bin/sh", X_OK);
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: optional init hook access=%d shell=%d\n", init_script_access,
                 shell_access);
        printf("%s", message);
    }
    if (init_script_access != 0 || shell_access != 0)
        return true;

    constexpr size_t max_script_bytes = 64 * 1024;
    constexpr size_t max_read_bytes = NA_CHANNEL_MAX_MESSAGE_BYTES - 16;
    auto *script = static_cast<char *>(malloc(max_script_bytes + 1));
    if (script == nullptr)
    {
        printf("init: /etc/init.sh allocation failed\n");
        return false;
    }
    const int script_fd = open("/etc/init.sh", O_RDONLY);
    if (script_fd < 0)
    {
        printf("init: /etc/init.sh open failed\n");
        free(script);
        return false;
    }
    size_t script_bytes = 0;
    bool script_read_failed = false;
    while (script_bytes < max_script_bytes)
    {
        const size_t request_bytes =
            (max_script_bytes - script_bytes) < max_read_bytes ? (max_script_bytes - script_bytes) : max_read_bytes;
        const ssize_t count = read(script_fd, script + script_bytes, request_bytes);
        if (count < 0)
        {
            script_read_failed = true;
            break;
        }
        if (count == 0)
            break;
        script_bytes += static_cast<size_t>(count);
    }
    if (!script_read_failed && script_bytes == max_script_bytes)
    {
        char probe = 0;
        script_read_failed = read(script_fd, &probe, 1) != 0;
    }
    close(script_fd);
    if (script_read_failed)
    {
        char message[112]{};
        snprintf(message, sizeof(message), "init: /etc/init.sh read failed or too large errno=%d bytes=%u\n", errno,
                 static_cast<unsigned>(script_bytes));
        printf("%s", message);
        free(script);
        return false;
    }
    script[script_bytes] = 0;
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: /etc/init.sh loaded bytes=%u\n", static_cast<unsigned>(script_bytes));
        printf("%s", message);
    }

    // The production hook is intentionally empty.  Starting an interpreter for
    // it costs a BusyBox materialisation and a process round trip on every
    // boot, so skip the spawn when the script has no command to run.
    if (!script_has_work(script))
    {
        printf("init: /etc/init.sh has no commands; skipping the hook\n");
        free(script);
        return true;
    }

    // Pass the bytes through `sh -c` instead of asking the child shell to
    // reopen `/etc/init.sh`. The latter can observe the archive entry's
    // metadata through a cloned Directory endpoint but lose its payload while
    // the early userland root is still being handed off.
    char *argv[] = {const_cast<char *>("sh"), const_cast<char *>("-c"), script, const_cast<char *>("/etc/init.sh"),
                    nullptr};
    pid_t pid = -1;
    na_handle_t process = NA_HANDLE_INVALID;
    const int spawn_error = naos_native_spawn_stdio_deferred(&pid, &process, "/bin/busybox", argv, environ,
                                                             STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
    {
        char message[112]{};
        snprintf(message, sizeof(message), "init: optional init hook spawn error=%d pid=%d\n", spawn_error, pid);
        printf("%s", message);
    }
    free(script);
    if (spawn_error != 0 || pid <= 0)
    {
        printf("init: /etc/init.sh spawn failed\n");
        return false;
    }

    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: /etc/init.sh spawned pid=%d\n", pid);
        printf("%s", message);
    }

    const int start_error = naos_native_start_process(process);
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: optional init hook start error=%d\n", start_error);
        printf("%s", message);
    }
    if (start_error != 0)
    {
        printf("init: /etc/init.sh start failed\n");
        (void)_na_handle_close(process);
        return false;
    }

    int status = 0;
    const pid_t waited = waitpid(pid, &status, 0);
    {
        char message[112]{};
        snprintf(message, sizeof(message), "init: optional init hook wait pid=%d status=%d\n", waited, status);
        printf("%s", message);
    }
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        char message[128]{};
        snprintf(message, sizeof(message), "init: /etc/init.sh failed pid=%d waited=%d status=%d errno=%d\n", pid,
                 waited, status, errno);
        printf("%s", message);
        (void)_na_handle_close(process);
        return false;
    }
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: /etc/init.sh completed pid=%d status=%d\n", pid, status);
        printf("%s", message);
    }
    (void)_na_handle_close(process);
    return true;
}

// Exercise one ordinary userland bootstrap before attaching the interactive
// terminal.  A cold vfsd/ttyd handoff can otherwise make the first deferred
// shell executable lookup observe a transient peer close; this barrier is a
// real service readiness check and never runs the smoke suites.
bool run_startup_barrier()
{
    // The readiness being established is "the shell executable can be opened
    // through the committed root".  Probe exactly that instead of spawning a
    // throwaway interpreter to discover it: an open plus close exercises the
    // same lookup the shell bootstrap will perform, and costs no process.
    for (int attempt = 0; attempt < 10; attempt++)
    {
        const int executable = open("/bin/busybox", O_RDONLY);
        if (executable >= 0)
        {
            close(executable);
            return true;
        }
        usleep(20'000);
    }
    return false;
}

void run_user_shell(pid_t initial_console)
{
    pid_t console = initial_console;
    pid_t shell = -1;
    int slave = -1;
    bool framebuffer_notice_logged = false;

    printf("init: frontend supervision started\n");
    for (;;)
    {
        if (g_ttyd_pid <= 0)
        {
            printf("init: ttyd unavailable; restarting frontend\n");
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
            printf("init: consoled restart scheduled\n");
        }
        if (reap_process(&shell, "user shell"))
        {
            close(slave);
            slave = -1;
            printf("init: user shell restart scheduled\n");
        }
        bool framebuffer_ready = false;
        if (console <= 0)
        {
            framebuffer_ready = framebuffer_available();
            if (!framebuffer_ready)
            {
                // Framebuffer/DevFS is optional during the VFS migration. A
                // ttyd-backed shell remains useful without the graphical
                // consoled frontend; do not spin forever waiting for a display service.
                if (!framebuffer_notice_logged)
                {
                    printf("init: framebuffer unavailable; using ttyd-only shell\n");
                    framebuffer_notice_logged = true;
                }
            }
            else if (!spawn_consoled_process(&console))
            {
                sleep(5);
                continue;
            }
        }
        if (console > 0 && shell <= 0)
        {
            // A newly spawned frontend needs to claim the console master
            // before the first slave reader starts, otherwise ash sees an
            // initial EOF and exits cleanly.
            sleep(2);
        }

        if (console > 0 && shell <= 0)
            printf("init: spawning user shell\n");

        if (shell <= 0 && (console > 0 || !framebuffer_ready) && !spawn_user_shell(&shell, &slave))
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
    // init is a fixed early boot module.  It may receive stdio and
    // ServiceDirectory before the filesystem worker is ready, but it must not
    // touch a path or spawn a normal service until vfsd has published the
    // committed root route.
    const int root_error = naos_native_install_root_namespace();
    (void)setvbuf(stdout, nullptr, _IONBF, 0);
    if (root_error != 0)
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: root route unavailable error=%d\n", root_error);
        printf("%s", message);
        return;
    }
    printf("init: committed root route ready\n");

    // init owns the first interactive session.  The kernel only permits a
    // session leader to acquire a controlling terminal; bootstrapping init
    // can inherit the module launcher's process group, so make the session
    // explicit before ttyd and the shell are started.  EPERM means the
    // launcher already made us a session leader and is harmless here.
    const pid_t session = setsid();
    if (session < 0 && errno != EPERM)
    {
        char message[96]{};
        snprintf(message, sizeof(message), "init: session setup failed errno=%d\n", errno);
        printf("%s", message);
    }
    else
        printf("init: session ready\n");
    if (!start_ttyd())
        return;
    printf("init: ttyd ready; rootfsd ownership remains with vfsd\n");

    // The graphical frontend is independent of the optional init hook and
    // the ordinary shell readiness barrier. Start it as soon as ttyd is
    // usable so the kernel VGA console can hand the scanout to consoled
    // without waiting for another large executable materialization.
    pid_t initial_console = -1;
    if (framebuffer_available() && !spawn_consoled_process(&initial_console))
        printf("init: pre-barrier consoled spawn failed; will retry later\n");
    (void)run_optional_init_script();
    printf("init: running startup barrier\n");
    if (!run_startup_barrier())
        printf("init: startup barrier failed; continuing with shell supervision\n");
    printf("init: startup barrier returned\n");
    run_user_shell(initial_console);
}
