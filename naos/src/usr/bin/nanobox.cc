#include <cstdint>
#include <cstdio>
#include <cstring>
uint64_t __dso_handle;

int nsh(int argc, char **argv);

int main(int argc, char **argv)
{
    if (strcmp(argv[0], "nsh") == 0)
    {
        return nsh(argc - 1, argv + 1);
    }
    printf(R"(nanobox: command line tool box for NanoOs
)");
    return 0;
}
