#include "common.hpp"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

[[gnu::weak]] void *__dso_handle;

extern "C" void main(int argc, char **argv)
{
    while (1)
    {
        printf("fork nsh...\n");
        int pid = fork();
        if (pid == 0)
        {
            // child process
            // run shell
            int ret = execl("/bin/nanobox", "nsh", nullptr);
            printf("execute process %d return %d\n", pid, ret);
            sleep(5);
            exit(ret);
        }
        else if (pid > 0)
        {
            int status = 0;
            int ret = waitpid(pid, &status, 0);
            if (ret != 0) 
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
            printf("fork process fail %d\n", pid);
        }
    }
}