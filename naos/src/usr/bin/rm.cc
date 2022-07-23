#include <unistd.h>
int rm(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            unlink(argv[i]);
        }
    }
    return 0;
}