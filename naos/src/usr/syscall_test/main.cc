#include "../common/arch/lib.hpp"
const char file_content[] = "init file write context";

void print(const char *str)
{
    unsigned long len = 0;
    auto p = str;
    while (*p++ != 0)
        ;
    len = p - str;
    write(STDOUT, str, len, 0);
}

int strcmp(const char *str1, const char *str2)
{
    if (str1 == str2)
        return true;
    if (!str1 || !str2)
        return false;

    while (1)
    {
        if (*str1 != *str2)
            return (int)*str1 - *str2;
        if (*str1 == 0)
            return 0;
        str1++;
        str2++;
    }
}

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
        print("thread2 wake up\n");
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
            unsigned long pos = read(fd, buffer, 32, 0);
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
    write(fd, file_content, sizeof(file_content), 0);
    close(fd);
}

void test_file(const char *file_path)
{
    int fd = open(file_path, OPEN_MODE_READ | OPEN_MODE_BIN, 0);
    char fc[sizeof(file_content)];
    lseek(fd, 0, LSEEK_MODE_BEGIN);
    if (read(fd, fc, sizeof(file_content), 0) < (long)sizeof(file_content))
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
    mkdir("/syscall_test/");
    mkdir("/syscall_test/ft2");
    chdir("/syscall_test");
    symbolink("/ft", "/syscall_test", 0);
    symbolink("./ft2/text.link", "./text", 0);

    create("./text1");
    write_file("./text1");

    rename("./text1", "./text");
    link("the_text", "./text");
    test_file("/syscall_test/the_text");
    test_file("/ft/text");
    test_file("/ft/ft2/text.link");
    unlink("text");
    unlink("the_text");
    unlink("/ft/ft2/text.link");
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

const char msg_str[] = "hi, message sent from msg queue.";

void read_test_message_queue(long msg_id)
{
    char buffer[sizeof(msg_str) + 1];
    while (1)
    {
        long ret = 0;
        auto len = read_msg_queue(msg_id, 1, &buffer, sizeof(msg_str), MSGQUEUE_FLAGS_NOBLOCKOTHER);

        if (len == ECONTI)
        {
            if (read_msg_queue(msg_id, 2, &ret, sizeof(ret), MSGQUEUE_FLAGS_NOBLOCK) == sizeof(ret))
            {
                print("message queue thread test end.");
                exit_thread(ret);
            }
            print("read from message queue failed. \n");
            exit(-2);
        }
    }
}

void test_message_queue()
{
    print("message queue testing\n");
    auto msg_id = create_msg_queue(2, 100);
    if (msg_id < 0)
    {
        print("Can't create message queue.");
        exit_thread(-1);
    }
    auto tid = create_thread((void *)read_test_message_queue, msg_id, 0);
    for (int i = 0; i < 20; i++)
    {
        write_msg_queue(msg_id, 1, msg_str, sizeof(msg_str), 0);
    }
    long exit_ret = 1;
    write_msg_queue(msg_id, 2, &exit_ret, sizeof(exit_ret), 0);
    long ret;
    join(tid, &ret);
    close_msg_queue(msg_id);

    if (ret != exit_ret)
    {
        print("message queue test failed\n");
        exit_thread(-1);
    }
    print("message queue tested.\n");
}
const char msg_pipe_str[] = "hi, message sent from pipe.";

void pipe_thread(long in)
{
    char buffer[sizeof(msg_pipe_str) + 1];
    print("pipe try read data...\n");
    long i = 0;
    while (true)
    {
        auto len = read(in, buffer, sizeof(msg_pipe_str), 0);
        if (len == EOF)
        {
            if (i != sizeof(msg_pipe_str) * 10)
            {
                break;
            }
            close(in);
            exit_thread(0);
        }
        i += len;
    }
    print("pipe test failed.");
    close(in);
    exit(-1);
}

void test_pipe()
{
    print("pipe testing\n");
    int in, out;
    if (get_pipe(&out, &in) != OK)
    {
        print("pipe test failed.");
        exit_thread(-1);
    }

    auto tid = create_thread((void *)pipe_thread, in, 0);
    print("pipe write to input\n");
    for (int i = 0; i < 10; i++)
    {
        write(out, msg_pipe_str, sizeof(msg_pipe_str), 0);
    }
    close(out);
    join(tid, nullptr);
    print("pipe tested\n");
}

const char *path = "/fifo_test";

void fifo_thread()
{
    auto fd = open(path, OPEN_MODE_READ | OPEN_MODE_BIN, 0);
    if (fd < OK)
    {
        print("fifo test failed. can't open fifo");
        exit_thread(-1);
    }

    char buffer[sizeof(msg_pipe_str) + 1];

    for (int i = 0; i < 10; i++)
    {
        auto len = read(fd, buffer, sizeof(msg_pipe_str), 0);
        if (len == EOF)
        {
            close(fd);
            exit_thread(0);
        }
        buffer[len] = 0;
        if (len != sizeof(msg_pipe_str) || strcmp(buffer, msg_pipe_str) != 0)
        {
            close(fd);
            exit(-1);
        }
    }
    close(fd);
    exit_thread(0);
}

void test_fifo()
{
    print("fifo testing\n");
    auto fd = create_fifo(path, 0);
    if (fd < 0)
    {
        print("fifo test failed. can't open fifo");
        exit_thread(-1);
    }

    auto tid = create_thread((void *)fifo_thread, 0, 0);

    for (int i = 0; i < 10; i++)
    {
        write(fd, msg_pipe_str, sizeof(msg_pipe_str), 0);
    }
    close(fd);
    join(tid, nullptr);
    unlink(path);
    print("fifo tested\n");
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

void sighandler()
{
    int num;
    sigwait(&num, nullptr);
    if (num == SIGINT)
        print("signal SIGINT handled\n");
    exit_thread(0);
}

extern "C" void _start(int argc, char **argv)
{
    sig_mask_t mask;
    sig_mask_init(mask);
    sig_mask_set(mask, SIGINT);

    sigmask(SIGOPT_OR, &mask, nullptr, nullptr);
    auto sigid = create_thread((void *)sighandler, 0, 0);
    // __asm__ __volatile__("INT $3  \n\t" : : : "memory");
    print("Begin tests.\n");
    auto tid = test_thread();
    test_fs();
    test_memory();
    test_message_queue();
    test_pipe();
    test_fifo();
    long ret;
    print("join thread2\n");
    join(tid, &ret);
    raise(SIGINT, nullptr);
    join(sigid, nullptr);
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