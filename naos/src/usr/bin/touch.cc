#include <fcntl.h>
#include <stdio.h>
int touch(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            if (creat(argv[i], 0) != 0)
            {
                printf("touch file %s fail\n", argv[i]);
            }
        }
    }
    return 0;
}