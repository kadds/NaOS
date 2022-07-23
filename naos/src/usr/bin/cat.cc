#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int cat(int argc, char **argv)
{
    int output_fd = STDOUT_FILENO;
    int input_fd = STDIN_FILENO;
    if (argc > 1)
    {
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0)
            exit(-1);
        input_fd = fd;
    }
    char data[1024];
    long read_size;
    while ((read_size = read(input_fd, data, 1024)) != EOF)
    {
        write(output_fd, data, read_size);
    }
    if (input_fd != STDIN_FILENO)
        close(input_fd);
    return 0;
}