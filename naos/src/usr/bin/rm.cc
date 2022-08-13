#include <stdio.h>
#include <unistd.h>
int rm(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            if (unlink(argv[i]) != 0)
            {
                printf("rm file %s fail\n", argv[i]);
            }
        }
    }
    return 0;
}
