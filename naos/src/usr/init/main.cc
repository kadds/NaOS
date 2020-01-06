#include "../common/arch/lib.hpp"
const char hello[] = "userland init\e[32;44mhi,\e[0minit! \e[1;38;2;255;0;25;49mcolor text\e[0m\n";
const char start[] = "running syscall test ...\n";
const char failed[] = "syscall_test isn't pass \n";
const char ok[] = "syscall_test exit with 0! \n";
const char promot[] = "kernel>";

extern "C" void _start(char *args)
{
    write(STDOUT, hello, sizeof(hello), 0);
    write(STDOUT, start, sizeof(start), 0);

    unsigned long pid = create_process("/bin/syscall_test", nullptr, 0);
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
    char cmd[128];
    const char tip[] = "input command: ";
    char t = '\n';
    while (1)
    {
        write(STDOUT, promot, sizeof(promot), 0);
        int len = read(STDIN, cmd, 127, 0);
        if (len == 0)
            continue;
        cmd[len] = 0;
        write(STDOUT, tip, sizeof(tip), 0);
        write(STDOUT, cmd, len, 0);
        write(STDOUT, &t, 1, 0);
    }
}