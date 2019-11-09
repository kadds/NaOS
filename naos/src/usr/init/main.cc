#include "usr/init/main.hpp"

extern "C" void _start()
{
    sys_none();
    print("userland init");
    print("\e[32;44mhi,\e[0minit! \e[1;38;2;255;0;25;49mcolor text\e[0m");
    int fd = open("/data/hello", 0, 0);
    if (fd < 0)
    {
        print("Can't find file /data/hello\n");
    }
    else
    {
        char buffer[129];
        while (read(fd, buffer, 128) > 0)
        {
            buffer[128] = 0;
            print(buffer);
        }
        close(fd);
    }
    while (1)
    {
        sleep(1000);
        print("init second \n");
        // __asm__ __volatile__("INT $3  \n\t" : : : "memory");
    }
}