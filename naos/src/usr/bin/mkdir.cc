
#include <stdio.h>
#include <sys/stat.h>

int mkdir(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            if (mkdir(argv[i], 0) != 0)
            {
                printf("mkdir %s fail\n", argv[i]);
            }
        }
    }
    return 0;
}