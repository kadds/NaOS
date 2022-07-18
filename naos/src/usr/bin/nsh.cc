#include <cstdio>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
constexpr int max_command_chars = 8192;
int nsh(int argc, char **argv)
{
    char *cmd = reinterpret_cast<char *>(malloc(max_command_chars));
    char *execute_file = reinterpret_cast<char *>(malloc(512));
    while (true)
    {
        size_t n = max_command_chars;
        ssize_t bytes = 0;
        printf("nsh>");
        fflush(stdout);
        if (bytes = getline(&cmd, &n, stdin); bytes == -1)
        {
            printf("no line\n");
            continue;
        }
        if (bytes == 1)
        {
            continue;
        }
        if (bytes > 1 && cmd[bytes - 1] == '\n')
        {
            cmd[--bytes] = '\0';
        }
        if (strcmp(cmd, "help") == 0)
        {
            printf(R"(nsh: nano shell: 
    command args            
)");
            continue;
        }

        auto command_end = strchr(cmd, ' ');
        if (command_end != nullptr)
        {
            if (command_end - cmd > 512)
            {
                printf("command char overflow\n");
                continue;
            }
            memcpy(execute_file, cmd, command_end - cmd);
        }
        else
        {
            strcpy(execute_file, cmd);
        }
        if (strcmp(execute_file, "cd") == 0)
        {
            chdir(command_end);

            continue;
        }
        if (strcmp(execute_file, "pwd") == 0)
        {
            if (getcwd(execute_file, 512) == 0)
            {
                printf("%s\n", execute_file);
            }
            else
            {
                printf("execute fail\n");
            }
            continue;
        }

        if (access(execute_file, F_OK | X_OK | R_OK) != 0)
        {
            printf("command '%s' not found\n", execute_file);
            continue;
        }

        int pid = fork();
        if (pid == 0)
        {
            int ret = execve(execute_file, argv, nullptr);
            exit(ret);
        }
        else if (pid > 0)
        {
            int status = 0;
            waitpid(pid, &status, 0);
            if (status != 0)
            {
                // todo print it
            }
        }
    }

    return 0;
}