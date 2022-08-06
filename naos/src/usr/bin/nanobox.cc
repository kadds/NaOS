#include "freelibcxx/allocator.hpp"
#include "freelibcxx/string.hpp"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern char __executable_start[];

[[gnu::weak]] void *__dso_handle;
[[gnu::weak]] void *__ehdr_start = __executable_start;

#define entry(name) int name(int argc, char **argv)

entry(nsh);
entry(cat);
entry(ls);
entry(mkdir);
entry(rmdir);
entry(touch);
entry(rm);
entry(env);

using entry_function = int(int, char **);
using namespace freelibcxx;

DefaultAllocator alloc;

extern "C" int main(int argc, char **argv)
{
    vector<string> args(&alloc);
    for (int i = 0; i < argc; i++)
    {
        args.push_back(string_view(argv[i]).to_string(&alloc));
    }

    if (args[0] == "nanobox")
    {
        printf("args %s", args[1].data());
    }

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
    else if (strstr(cmd, "env"))
    {
        return env(argc, argv);
    }
    else
    {
        printf("nanobox: unknown command %s\n", cmd);
    }
    return 0;
}
