#include "../common/arch/lib.hpp"
const char file_content[] = "init file write context";

void thread(int arg)
{
    if (arg != 2)
    {
        print("thread param id!=2\n");
        exit_thread(1, 0);
    }
    for (int i = 0; i < 2; i++)
    {
        print("thread2 sleep\n");
        sleep(100);
    }
    print("thread2 exit\n");
    exit_thread(0, 0);
}

void test_file()
{
    int fd = open("/data/hello", 0, 0);
    if (fd < 0)
    {
        print("Can't find file /data/hello\n");
    }
    else
    {
        char buffer[33];
        while (1)
        {
            unsigned long pos = read(fd, buffer, 32);
            if (pos > 0)
            {
                buffer[pos] = 0;
                print(buffer);
            }
            else
                break;
        }

        close(fd);
    }

    fd = open("/data/init.text", 2 | 4, 1 | 2);
    write(fd, file_content, sizeof(file_content));
    char fc[52];
    lseek(fd, 0, 1);
    read(fd, fc, 52);
    for (unsigned int i = 0; i < sizeof(file_content); i++)
    {
        if (file_content[i] != fc[i])
        {
            print("syscall write/read failed!");
            exit(-1);
        }
    }
}

void test_fs() {}

unsigned long test_thread() { return create_thread((void *)thread, 2, 0); }

void test_memory()
{
    void *head = (void *)sbrk(0);
    sbrk(1024);
    void *new_head = (void *)sbrk(0);

    *(char *)head = 'A';

    brk((unsigned long)head);
}

extern "C" void _start(char *args)
{
    // __asm__ __volatile__("INT $3  \n\t" : : : "memory");
    print("Begin tests.\n");
    test_memory();
    unsigned long tid = test_thread();
    test_file();
    test_fs();
    long ret;
    join(tid, &ret);
    if (ret != 0)
    {
        print("Some test tests failed.\n");
        exit(-1);
    }
    else
    {
        print("All test tests succeeded.\n");
        exit(0);
    }
}