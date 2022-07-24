
#include <stdio.h>
#include <stdlib.h>
extern char **environ;

int env(int argc, char **argv)
{
    auto envp = environ;
    while (*envp != nullptr)
    {
        auto env = *envp;
        printf("%s\n", env);
        envp++;
    }
    return 0;
}