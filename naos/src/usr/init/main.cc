#include "usr/init/main.hpp"

extern "C" __attribute__((section(".start"))) void main()
{
    sys_none();
    print("userland init");
    while (1)
        ;
}