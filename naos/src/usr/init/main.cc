#include "../common/arch/lib.hpp"
const char hello[] = "userland init\e[32;44mhi,\e[0minit! \e[1;38;2;255;0;25;49mcolor text\e[0m\n";
const char start[] = "running syscall test ...\n";
const char failed[] = "syscall_test isn't pass \n";
const char ok[] = "syscall_test exit with 0! \n";
const char shell_failed[] = "execute nsh failed, check rootfs image and memory size in system.\nhlt at init.\n";
const char startup_shell[] = "startup nsh...\n";

extern "C" void _start(char *args)
{
    write(STDOUT, hello, sizeof(hello), 0);
    write(STDOUT, start, sizeof(start), 0);

    // run system test
    auto pid = create_process("/bin/syscall_test", nullptr, 0);
    long ret;
    if (wait_process(pid, &ret) == 0)
    {
        if (ret != 0)
        {
            write(STDOUT, failed, sizeof(failed), 0);
            while (1)
            {
                sleep(1000);
            }
        }
        write(STDOUT, ok, sizeof(ok), 0);
    }

    write(STDOUT, startup_shell, sizeof(startup_shell), 0);

    // run shell
    while (1)
    {
        auto pid_nsh = create_process("/bin/nsh", nullptr, 0);
        if (pid_nsh <= 0)
        {
            write(STDOUT, shell_failed, sizeof(shell_failed), 0);
            while (1)
            {
                sleep(1000000000);
            }
        }
        wait_process(pid_nsh, nullptr);
    }
}