#include "../common/arch/lib.hpp"
const char file_content[] = "init file write context";

void thread(int arg)
{
    print("thread sleep testing\n");

    if (arg != 2)
    {
        print("thread param id!=2\n");
        exit_thread(1);
    }
    for (int i = 0; i < 2; i++)
    {
        print("thread2 sleep\n");
        sleep(1000);
        print("thread2 sleep end\n");
    }
    print("thread2 exit\n");
    exit_thread(0);
    for (;;)
        print("thread2 can't exit\n");
}

bool print_file(const char *file_path)
{
    int fd = open(file_path, OPEN_MODE_READ | OPEN_MODE_BIN, 0);
    if (fd < 0)
    {
        print("Can't find file ");
        print(file_path);
        print("\n");
        return false;
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
    return true;
}

void write_file(const char *file_path)
{
    int fd = open(file_path, OPEN_MODE_READ | OPEN_MODE_WRITE | OPEN_MODE_BIN, 0);
    write(fd, file_content, sizeof(file_content));
    close(fd);
}

void test_file(const char *file_path)
{
    int fd = open(file_path, OPEN_MODE_READ | OPEN_MODE_BIN, 0);
    char fc[sizeof(file_content)];
    lseek(fd, 0, LSEEK_MODE_BEGIN);
    if (read(fd, fc, sizeof(file_content)) < sizeof(file_content))
    {
        print("syscall write/read failed! file:");
        print(file_path);
        print("\n");
        exit_thread(-1);
    }
    for (unsigned int i = 0; i < sizeof(file_content); i++)
    {
        if (file_content[i] != fc[i])
        {
            print("syscall write/read failed! file:");
            print(file_path);
            print("\n");
            exit_thread(-1);
        }
    }
    close(fd);
}

void test_fs()
{
    print("file system testing\n");

    print_file("/data/hello");
    char path[256];
    current_dir(path, 255);
    print("current work dir:");
    path[255] = 0;
    print(path);
    print("\n");
    mkdir("/tmp/");
    mount("", "/tmp/", "ramfs", 0, nullptr, 0x10000);
    chroot("/tmp");
    mkdir("/future_test/");
    mkdir("/future_test/ft2");
    chdir("/future_test");
    symbolink("/ft", "/future_test", 0);
    symbolink("./ft2/text.link", "./text", 0);

    create("./text1");
    write_file("./text1");

    rename("./text1", "./text");
    link("the_text", "./text");
    test_file("/future_test/the_text");
    test_file("/ft/text");
    test_file("/ft/ft2/text.link");
    unlink("text");
    unlink("the_text");
    unlink("/ft/ft2");
    unlink("/ft");

    int fd = open("text", OPEN_MODE_READ, 0);
    if (fd >= 0)
    {
        print("Can't delete file text ");
        print("\n");
        exit_thread(1);
    }

    chroot("/../");
    chdir("/");
    umount("/tmp");
    print("file system tested\n");
}

unsigned long test_thread()
{
    print("thread testing\n");
    return create_thread((void *)thread, 2, 0);
}

void test_memory()
{
    print("memory testing\n");
    void *head = (void *)sbrk(0);
    sbrk(1024);

    *(char *)head = 'A';

    brk((unsigned long)head);

    void *p = mmap(0, 0, 0, 2048, MMAP_READ | MMAP_WRITE);
    *(char *)p = 'A';
    mumap(p);
    print("memory tested.\n");
}

void sighandler(int sig, long error, long code, long status)
{
    print("signal handle\n");
    sigreturn(0);
}

extern "C" void _start(char *args)
{
    sigaction(SIGINT, sighandler, 0, 0);
    // __asm__ __volatile__("INT $3  \n\t" : : : "memory");
    print("Begin tests.\n");

    unsigned long tid = test_thread();
    test_fs();
    test_memory();
    long ret;
    print("join thread2\n");
    join(tid, &ret);
    raise(SIGINT, 0, 0, 0);
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