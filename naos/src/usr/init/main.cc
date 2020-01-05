#include "../common/arch/lib.hpp"

extern "C" void _start(char *args)
{
    print("userland init");
    print("\e[32;44mhi,\e[0minit! \e[1;38;2;255;0;25;49mcolor text\e[0m\n");
    print("running syscall test ...\n");
    unsigned long pid = create_process("/bin/syscall_test", nullptr, 0);
    long ret;
    if (wait_process(pid, &ret) == 0)
    {
        if (ret != 0)
        {
            print("syscall_test isn't pass \n");
            while (1)
            {
                sleep(1000);
            }
        }
        print("syscall_test exit with 0! \n");
    }

    while (1)
    {
        char str[11];
        str[read(0, str, 10, 0)] = 0;
        write(1, str, 10, 0);
        print(str);
    }
}