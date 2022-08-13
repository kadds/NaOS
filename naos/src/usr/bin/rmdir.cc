#include <stdio.h>
#include <unistd.h>
int rmdir(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            if (rmdir(argv[i]) != 0)
            {
                printf("rmdir %s fail\n", argv[i]);
            }
        }
    }
    return 0;
}