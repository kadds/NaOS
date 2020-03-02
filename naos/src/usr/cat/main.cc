#include "../common/arch/lib.hpp"

extern "C" void _start(int argc, char **argv)
{
    int output_fd = STDOUT;
    int input_fd = STDIN;
    if (argc > 1)
    {
        int fd = open(argv[1], OPEN_MODE_READ, 0);
        if (fd < 0)
            return;
        input_fd = fd;
    }
    char data[1024];
    long read_size;
    while ((read_size = read(input_fd, data, 1024, 0)) != EOF)
    {
        write(output_fd, data, read_size, 0);
    }
    exit(0);
}