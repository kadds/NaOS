
#include <fcntl.h>
int touch(int argc, char **argv)
{
    if (argc > 1)
    {
        for (int i = 1; i < argc; i++)
        {
            creat(argv[i], 0);
        }
    }
    return 0;
}