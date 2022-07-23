
#include <sys/stat.h>

int mkdir(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            mkdir(argv[i], 0);
        }
    }
    return 0;
}