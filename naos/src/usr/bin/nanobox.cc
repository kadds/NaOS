#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint64_t __dso_handle;

#define entry(name) int name(int argc, char **argv)

entry(nsh);
entry(cat);
entry(ls);
entry(mkdir);
entry(rmdir);
entry(touch);
entry(rm);

extern "C" int main(int argc, char **argv)
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
    if (argc == 0 || cmd == nullptr)
    {
        printf(R"(nanobox: command line tool box for NanoOs
)");
        return 0;
    }
    if (strcmp(cmd, "nsh") == 0)
    {
        return nsh(argc, argv);
    }
    else if (strcmp(cmd, "cat") == 0)
    {
        return cat(argc, argv);
    }
    else if (strcmp(cmd, "ls") == 0)
    {
        return ls(argc, argv);
    }
    else if (strcmp(cmd, "mkdir") == 0)
    {
        return mkdir(argc, argv);
    }
    else if (strcmp(cmd, "rmdir") == 0)
    {
        return rmdir(argc, argv);
    }
    else if (strcmp(cmd, "touch") == 0)
    {
        return touch(argc, argv);
    }
    else if (strcmp(cmd, "rm") == 0)
    {
        return rm(argc, argv);
    }
    else
    {
        printf("unknown command %s\n", cmd);
    }
    return 0;
}
