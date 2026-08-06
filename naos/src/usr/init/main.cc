#include <naos/service_directory.hpp>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

[[gnu::weak]] void *__dso_handle;

extern "C" void main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    extern char **environ;

    int service_status = naos_service_register_fd("console", STDOUT_FILENO);
    printf("service directory register console: %d\n", service_status);
    if (service_status == 0)
    {
        na_handle_t resolved = NA_HANDLE_INVALID;
        service_status = naos_service_resolve("console", &resolved);
        printf("service directory resolve console: %d\n", service_status);
        if (resolved != NA_HANDLE_INVALID)
            (void)naos_handle_close(resolved);

        service_status = naos_service_unregister("console");
        printf("service directory unregister console: %d\n", service_status);
    }

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
