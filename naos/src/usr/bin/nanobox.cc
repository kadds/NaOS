#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint64_t __dso_handle;

int nsh(int argc, char **argv);

int main(int argc, char **argv)
{
    if (strstr(argv[0], "nanobox") != nullptr)
    {
        if (argc == 1)
        {
            argv[0] = nullptr;
        }
        else
        {
            argc--;
            argv++;
        }
    }
    char *cmd = argv[0];
    if (cmd == nullptr)
    {
        printf(R"(nanobox: command line tool box for NanoOs
)");
        // return 0;
    }
    if (strcmp(cmd, "nsh") == 0)
    {
        return nsh(argc, argv);
    }
    return 0;
}
