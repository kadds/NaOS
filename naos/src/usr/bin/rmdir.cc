#include <unistd.h>
int rmdir(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            rmdir(argv[i]);
        }
    }
    return 0;
}