#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void cat_fd(int fdin)
{
    char data[1024];
    long read_size;
    while ((read_size = read(fdin, data, 1024)) != EOF)
    {
        write(STDOUT_FILENO, data, read_size);
    }
}

int cat(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {

            int fd = open(argv[i], O_RDONLY);
            if (fd < 0)
            {
                printf("file %s not found\n", argv[i]);
                continue;
            }
            cat_fd(fd);
            close(fd);
        }
    }
    else
    {
        cat_fd(STDIN_FILENO);
    }
    return 0;
}