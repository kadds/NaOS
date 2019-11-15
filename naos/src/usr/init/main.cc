#include "../common/arch/lib.hpp"

extern "C" void _start(char *args)
{
    print("userland init");
    print("\e[32;44mhi,\e[0minit! \e[1;38;2;255;0;25;49mcolor text\e[0m\n");
    print("start future test ...\n");
    unsigned long pid = create_process("/bin/future_test", nullptr, 0);
    long ret;
    if (wait_process(pid, 0, &ret) != 0)
    {
        if (ret != 0)
        {
            print("future test failed! \n");
            while (1)
            {
                sleep(1000);
            }
        }
    }

    while (1)
    {
        sleep(1000);
        print("init second \n");
    }
}