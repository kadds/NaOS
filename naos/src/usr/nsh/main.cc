#include "../common/arch/lib.hpp"

const char promot[] = "NaOS>";
const char unknown_command[] = "unknown command ";

extern "C" void _start(char *args)
{
    auto ptr = sbrk(4096);
    if (ptr == EFAILED)
    {
        write(STDERR, "sbrk failed nsh.", 12, 0);
        while (1)
        {
            sleep(100000000);
        }
    }
    char *cmd = (char *)ptr;
    char t = '\n';

    while (1)
    {
        write(STDOUT, promot, sizeof(promot), 0);
        int len = read(STDIN, cmd, 4096, 0);
        if (len == 0)
            continue;
        cmd[len] = 0;
        write(STDOUT, unknown_command, sizeof(unknown_command), 0);
        write(STDOUT, cmd, len, 0);
        write(STDOUT, &t, 1, 0);
    }
}