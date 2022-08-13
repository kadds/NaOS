
#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "nanobox.hpp"

#include <asm-generic/errno-base.h>
#include <fcntl.h>
#include <stdio.h>

#include <bits/posix/posix_stdlib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

const char *helpstr = R"(nsh: nano shell: 
    cd <path>
    pwd
    exit <code>
    echo <args>
    export ENV=VALUE
    command <args>
    command <args> > stdout
    command <args> 1> stdout
    command <args> 2> stderr
    command <args> 1> stdout 2> stderr
    command <args> 1> stdout 2>&1
)";

constexpr int max_command_chars = 8192;
using namespace freelibcxx;

bool parse_args(const_string_view input, vector<string> &args, FILE *&stdout, FILE *&stderr)
{
    stdout = ::stdout;
    stderr = ::stderr;
    if (input.size() == 0)
    {
        return false;
    }

    const char *quote = nullptr;
    const char *double_quote = nullptr;
    bool escape = false;

    string cur(&alloc);
    const char *p = input.data();

    while (*p != '\0')
    {
        char ch = *p;
        if (!escape && ch == '\'')
        {
            if (quote != nullptr)
            {
                args.push_back(cur);
                cur.clear();
                quote = nullptr;
            }
            else
            {
                quote = p;
            }
        }
        else if (!escape && ch == '"')
        {
            if (double_quote != nullptr)
            {
                args.push_back(cur);
                cur.clear();

                double_quote = nullptr;
            }
            else
            {
                double_quote = p;
            }
        }
        else if (ch == '\\')
        {
            if (escape)
            {
                cur += ch;
                escape = false;
            }
            else
            {
                escape = true;
            }
        }
        else if (ch == ' ' && double_quote == nullptr && quote == nullptr && !escape)
        {
            args.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += ch;
        }

        p++;
    }
    if (cur.size() > 0)
    {
        args.push_back(std::move(cur));
    }

    // check redirection

    bool redirect = false;
    for (size_t i = 0; i < args.size();)
    {
        auto str = args[i].const_view();
        str.trim();
        if (str.size() == 0)
        {
            args.remove_at(i);
            continue;
        }

        if (str == "1>" || str == ">")
        {
            redirect = true;
            // query stdout file
            if (i + 1 < args.size())
            {
                auto &file = args[i + 1];
                auto f = fopen(file.data(), "w");
                if (f != nullptr)
                {
                    stdout = f;
                }
                args.remove_n_at(i, 2);
            }
        }
        else if (str == "2>")
        {
            redirect = true;
            if (i + 1 < args.size())
            {
                auto &file = args[i + 1];
                auto f = fopen(file.data(), "w");
                if (f != nullptr)
                {
                    stderr = f;
                }
                args.remove_n_at(i, 2);
            }
        }
        else if (str == "2>&1")
        {
            redirect = true;
            stderr = stdout;
            args.remove_n_at(i, 1);
        }
        else
        {
            if (redirect)
            {
                // ignore rest args
                args.truncate(i);
            }
        }
        i++;
    }
    return true;
}

int wait_input(FILE *file, size_t max, char *&input)
{
    ssize_t bytes = 0;
    size_t n = max;
    if (bytes = getline(&input, &n, file); bytes == -1)
    {
        return EOF;
    }
    if (bytes == 1)
    {
        if (input[0] == '\n')
        {
            input[0] = 0;
        }
        return 0;
    }
    if (bytes > 1 && input[bytes - 1] == '\n')
    {
        input[--bytes] = '\0';
    }
    return 0;
}

int echo(FILE *fout, vector<string> &args)
{
    for (size_t i = 1; i < args.size(); i++)
    {
        auto arg = args[i].const_view();
        while (true)
        {
            auto iter = arg.find('$');
            if (iter == arg.end() && iter + 1 != arg.end())
            {
                break;
            }
            if (iter - arg.begin() > 0)
            {
                auto val = arg.to_string(&alloc);
                printf("%s", val.data());
            }
            // find ${XXX} or $XXX
            auto next = iter + 1;
            auto view = arg.substr(iter - arg.begin());
            char end_ch = ' ';
            int offset = 1;
            if (*next == '{')
            {
                end_ch = '}';
                offset++;
            }
            // find end char
            auto end_iter = view.find(end_ch);
            auto env_key = view.substr(offset, end_iter - view.begin() - offset).to_string(&alloc);
            if (end_iter != view.end())
            {
                end_iter++;
            }
            iter = end_iter;
            // replace ENV
            auto value = getenv(env_key.data());
            if (value != nullptr)
            {
                fprintf(fout, "%s", value);
            }

            arg = arg.substr(iter - arg.begin());
        }
        if (arg.size() > 0)
        {
            auto val = arg.to_string(&alloc);
            fprintf(fout, "%s", val.data());
        }

        fprintf(fout, " ");
    }
    fprintf(fout, "\n");
    return 0;
}

int process(vector<string> &args, FILE *stdout, FILE *stderr)
{
    if (args.size() == 0)
    {
        return 0;
    }
    auto fout = stdout;

    const string &program = args[0];
    if (program == "help")
    {
        fprintf(fout, "%s", helpstr);
        return 0;
    }

    if (program == "cd")
    {
        chdir(args[1].data());
        return 0;
    }
    if (program == "exit")
    {
        if (args.size() > 1)
        {
            exit(atoi(args[1].data()));
        }
        else
        {
            exit(0);
        }
    }
    if (program == "pwd")
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)))
        {
            fprintf(fout, "%s\n", cwd);
        }
        else
        {
            fprintf(fout, "execute fail\n");
            return errno;
        }
        return 0;
    }
    if (program == "export")
    {
        auto arg = args[1].const_view();
        const_string_view view[2];
        auto n = arg.split_n<2>('=', view);

        if (n == 2)
        {
            string key = view->to_string(&alloc);
            setenv(key.data(), view[1].data(), 1);
            return 0;
        }
        else
        {
            fprintf(fout, "usage: export ENV=VALUE:VALUE2\n");
            return -1;
        }
    }
    if (program == "echo")
    {
        return echo(fout, args);
    }

    int pid = fork();
    if (pid == 0)
    {
        if (::stdout != stdout)
        {
            dup2(STDOUT_FILENO, fileno_unlocked(stdout));
        }
        if (::stderr != stderr)
        {
            dup2(STDERR_FILENO, fileno_unlocked(stderr));
        }

        vector<char *> args_array(&alloc);
        for (size_t i = 1; i < args.size(); i++)
        {
            args_array.push_back(args[i].data());
        }
        args_array.push_back(nullptr);

        int ret = execvp(program.data(), args_array.data());
        if (errno == EACCES)
        {
            printf("command '%s' not found\n", program.data());
        }
        else
        {
            printf("command '%s' returns %d\n", program.data(), ret);
        }
        exit(ret);
    }
    else if (pid > 0)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (status != 0)
        {
            // todo print it
            return status;
        }
    }
    return 0;
}

int process_single_line(FILE *file, string &ss)
{
    char *buf = ss.data();
    if (wait_input(file, max_command_chars, buf) != 0)
    {
        return EOF;
    }
    vector<string> args(&alloc);

    FILE *stdout, *stderr;
    if (parse_args(ss.const_view(), args, stdout, stderr))
    {
        process(args, stdout, stderr);
    }
    if (stdout != ::stdout)
    {
        fflush(stdout);
        fclose(stdout);
    }
    if (stderr != ::stderr)
    {
        fflush(stderr);
        fclose(stderr);
    }
    return 0;
}

int do_input()
{
    string ss(&alloc);
    ss.resize(max_command_chars);

    while (true)
    {
        printf("\e[32;1mnsh>\e[0m");
        fflush(stdout);
        if (process_single_line(stdin, ss) == EOF)
        {
            break;
        }
    }
    return 0;
}

void config_source(const char *path)
{
    string ss(&alloc);
    ss.resize(max_command_chars);

    auto file = fopen(path, "r");

    if (file == nullptr)
    {
        printf("source %s not found", path);
        return;
    }

    while (true)
    {
        if (process_single_line(file, ss) == EOF)
        {
            break;
        }
    }
}

int nsh(int argc, char **argv)
{
    // read shell config
    config_source("/etc/nshrc");

    do_input();

    return 0;
}