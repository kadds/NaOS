#include "usr/init/main.hpp"

extern "C" __attribute__((section(".start"))) void main()
{
    sys_none();
    print("userland init");
    while (1)
    {
        for (int i = 0; i < 100000; i++)
        {
        }
        // __asm__ __volatile__("INT $3  \n\t" : : : "memory");
    }
}