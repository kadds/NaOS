#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

[[gnu::weak]] void *__dso_handle;

extern "C" void main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    extern char **environ;
    while (1)
    {
        printf("spawn sh...\n");
        char *child_argv[] = {const_cast<char *>("sh"), nullptr};
        int pid = -1;
        const int spawn_error = posix_spawn(&pid, "/bin/sh", nullptr, nullptr, child_argv, environ);
        if (spawn_error == 0)
        {
            int status = 0;
            int ret = waitpid(pid, &status, 0);
            if (ret != pid)
            {
                printf("wait pid %d fail, error %d\n", pid, ret);
            }
            if (status != 0)
            {
                printf("process exit %d\n", status);
            }
            sleep(1);
        }
        else
        {
            printf("spawn process fail %d\n", spawn_error);
            sleep(5);
        }
    }
}
