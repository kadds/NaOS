#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
uint64_t __dso_handle;

void main(int argc, char **argv)
{
    while (1)
    {
        int pid = fork();
        if (pid == 0)
        {
            // child
            // run shell
            int ret = execl("/bin/nanobox", "nsh", nullptr);
            printf("execute process %d return %d\n", pid, ret);
            sleep(5);
            exit(ret);
        }
        else if (pid > 0)
        {
            int status = 0;
            waitpid(pid, &status, 0);
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