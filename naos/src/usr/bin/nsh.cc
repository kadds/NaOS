#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
constexpr int max_command_chars = 8192;

int parse_args(const char *args, char **&target)
{
    target = (char **)malloc(100);
    int num = 0;
    if (args == nullptr)
    {
        return num;
    }
    const char *quote = nullptr;
    const char *double_quote = nullptr;
    bool escape = false;
    char *cur = (char *)malloc(1000);
    int cur_len = 0;
    while (*args != '\0')
    {
        char ch = *args;
        if (!escape && ch == '\'')
        {
            if (quote != nullptr)
            {
                cur[cur_len] = '\0';
                target[num++] = cur;
                cur_len = 0;
                cur = (char *)malloc(1000);
                quote = nullptr;
            }
            else
            {
                quote = args;
            }
        }
        else if (!escape && ch == '"')
        {
            if (double_quote != nullptr)
            {
                cur[cur_len] = '\0';
                target[num++] = cur;
                cur_len = 0;
                cur = (char *)malloc(1000);
                double_quote = nullptr;
            }
            else
            {
                double_quote = args;
            }
        }
        else if (ch == '\\')
        {
            if (escape)
            {
                cur[cur_len] = ch;
                escape = false;
            }
            else
            {
                escape = true;
            }
        }
        else if (ch == ' ' && double_quote == nullptr && quote == nullptr && !escape)
        {
            cur[cur_len] = '\0';
            target[num++] = cur;
            cur_len = 0;
            cur = (char *)malloc(1000);
        }
        else
        {
            cur[cur_len++] = ch;
        }

        args++;
    }
    if (cur_len > 0)
    {
        cur[cur_len] = '\0';
        target[num++] = cur;
    }
    else
    {
        free(cur);
    }
    target[num] = nullptr;
    return num;
}

void clear_args(char **target, int num)
{
    for (int i = 0; i < num; i++)
    {
        free(target[i]);
    }
    free(target);
}

int wait_input(size_t max, char *&input)
{
    ssize_t bytes = 0;
    size_t n = max;
    if (bytes = getline(&input, &n, stdin); bytes == -1)
    {
        return -1;
    }
    if (bytes == 1)
    {
        return -1;
    }
    if (bytes > 1 && input[bytes - 1] == '\n')
    {
        input[--bytes] = '\0';
    }
    return 0;
}

int process(char **args, int arg_num)
{
    if (arg_num == 0)
    {
        return 0;
    }

    char *program = args[0];
    if (strcmp(program, "help") == 0)
    {
        printf(R"(nsh: nano shell: 
    command args            
)");
        return 0;
    }

    if (strcmp(program, "cd") == 0)
    {
        chdir(args[1]);
        return 0;
    }
    if (strcmp(program, "exit") == 0)
    {
        if (arg_num > 1)
        {
            exit(atoi(args[1]));
        }
        else
        {
            exit(0);
        }
    }
    if (strcmp(program, "pwd") == 0)
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)))
        {
            printf("%s\n", cwd);
        }
        else
        {
            printf("execute fail\n");
        }
        return 0;
    }

    if (access(program, F_OK | X_OK | R_OK) != 0)
    {
        printf("command '%s' not found\n", program);
        return 0;
    }

    int pid = fork();
    if (pid == 0)
    {
        int ret = execve(program, args + 1, nullptr);
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
    return 0;
}

int nsh(int argc, char **argv)
{
    char *cmd = reinterpret_cast<char *>(malloc(max_command_chars));
    while (true)
    {
        printf("nsh>");
        fflush(stdout);
        if (wait_input(max_command_chars, cmd) != 0)
        {
            continue;
        }

        char **args = nullptr;
        int arg_num = parse_args(cmd, args);
        process(args, arg_num);
        clear_args(args, arg_num);
    }

    return 0;
}