#include "nanobox.hpp"
#include <cstddef>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

[[gnu::weak]] void *__dso_handle;

#ifdef __GNUC__
extern char __executable_start[];
[[gnu::weak]] void *__ehdr_start = __executable_start;
#endif

using entry_function = int(int, char **);
#define entry(name) int name(int argc, char **argv)

entry(nsh);
entry(cat);
entry(ls);
entry(mkdir);
entry(rmdir);
entry(touch);
entry(rm);
entry(env);
entry(simd_test);

#define entry_p(name)                                                                                                  \
    {                                                                                                                  \
#name, name                                                                                                    \
    }

struct
{
    const char *name;
    entry_function *fn;
} static_commands[] = {
    entry_p(nsh),   entry_p(cat), entry_p(ls),  entry_p(mkdir),     entry_p(rmdir),
    entry_p(touch), entry_p(rm),  entry_p(env), entry_p(simd_test),
};

using namespace freelibcxx;
DefaultAllocator alloc;

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
    auto last_ch = strrchr(cmd, '/');
    if (last_ch != nullptr)
    {
        cmd = last_ch + 1;
    }

    for (auto s : static_commands)
    {
        if (strcmp(s.name, cmd) == 0)
        {
            return s.fn(argc, argv);
        }
    }

    printf("nanobox: unknown command '%s'\n", cmd);
    return 0;
}
