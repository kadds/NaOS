#include <cstdint>
#include <cstdio>
#include <cstring>
uint64_t __dso_handle;

int nsh(int argc, char **argv);

int main(int argc, char **argv)
{
    printf("nanobox\n");
    if (strcmp(argv[0], "nsh") == 0)
    {

        return nsh(argc - 1, argv + 1);
    }
    return 0;
}
