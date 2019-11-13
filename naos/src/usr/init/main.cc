#include "../common/arch/lib.hpp"

const char file_content[] = "init file write context";
void thread(int id)
{
    int fd = open("/data/init.text", 2 | 4, 1 | 2);
    write(fd, file_content, sizeof(file_content));
    char fc[52];
    lseek(fd, 0, 1);
    read(fd, fc, 52);
    for (int i = 0; i < sizeof(file_content); i++)
    {
        if (file_content[i] != fc[i])
        {
            print("syscall write/read failed!");
            break;
        }
    }

    if (id == 2)
        print("thread param id=2");

    for (int i = 0; i < 4; i++)
    {
        sleep(500);
        print("init thread second \n");
    }
    exit(1);
}

extern "C" void _start(char *args)
{
    void *head = (void *)sbrk(0);
    sbrk(1024);
    void *new_head = (void *)sbrk(0);

    *(char *)head = 'A';

    brk((unsigned long)head);

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
    create_thread((void *)thread, 2, 0);
    while (1)
    {
        sleep(1000);
        print("init second \n");
        // __asm__ __volatile__("INT $3  \n\t" : : : "memory");
    }
}