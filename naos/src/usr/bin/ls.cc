#include <dirent.h>
#include <stdio.h>
#include <string.h>

int ls(int argc, char **argv)
{
    const char *path = ".";
    if (argc > 1)
    {
        path = argv[1];
        ;
    }
    auto dir = opendir(path);
    if (dir == nullptr)
    {
        printf("fail open dir %s\n", path);
        return 0;
    }
    while (auto d = readdir(dir))
    {
        printf("%s\n", d->d_name);
    }
    return 0;
}