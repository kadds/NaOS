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
    if (strstr(cmd, "nsh"))
    {
        return nsh(argc, argv);
    }
    else if (strstr(cmd, "cat"))
    {
        return cat(argc, argv);
    }
    else if (strstr(cmd, "ls"))
    {
        return ls(argc, argv);
    }
    else if (strstr(cmd, "mkdir"))
    {
        return mkdir(argc, argv);
    }
    else if (strstr(cmd, "rmdir"))
    {
        return rmdir(argc, argv);
    }
    else if (strstr(cmd, "touch"))
    {
        return touch(argc, argv);
    }
    else if (strstr(cmd, "rm"))
    {
        return rm(argc, argv);
    }
    else
    {
        printf("unknown command %s\n", cmd);
    }
    return 0;
}
