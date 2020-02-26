#include "../common/arch/lib.hpp"

const char promot[] = "\e[92;48mNaOS\e[0m->";
const char unknown_command[] = "unknown command ";
const char help_msg[] = "some commands:\n"
                        "  \e[32mwhereis\e[0m  show current directory\n"
                        "  \e[32mcd\e[0m path  change current directory\n"
                        "  \e[32mmkdir\e[0m path  make directory\n"
                        "  \e[32mrmdir\e[0m path  remove directory\n"
                        "  \e[32mhelp\e[0m  show this info\n"
                        "enter process name to startup process.\n";

int start_with(const char *str, const char *prefix)
{
    if (str == nullptr || prefix == nullptr)
        return -1;
    int i = 0;
    while (1)
    {
        if (*prefix == '\0')
            return i;
        if (*str == '\0')
            return -1;
        if (*str != *prefix)
            return -1;
        str++;
        prefix++;
        i++;
    }
}

void print(const char *str)
{
    if (str == nullptr)
        return;
    unsigned long len = 0;
    auto p = str;
    while (*p++ != 0)
        ;
    len = p - str;
    write(STDOUT, str, len, 0);
}

int itoa(long num, char *buffer, int max_len)
{
    if (buffer == nullptr)
        return 0;
    if (max_len == 0)
        return 0;

    bool flag = false;
    if (num < 0)
    {
        flag = true;
        num = -num;
    }

    int i = 0;
    do
    {
        buffer[i++] = num % 10 + '0';
        num = num / 10;
    } while (num != 0 && i < max_len);

    if (flag)
    {
        if (buffer[i - 1] != '0')
        {
            buffer[i++] = '-';
            if (i >= max_len - 1)
            {
                buffer[i] = 0;
                return i;
            }
        }
    }

    for (int j = 0; j < i / 2; j++)
    {
        auto temp = buffer[j];
        buffer[j] = buffer[i - j - 1];
        buffer[i - j - 1] = temp;
    }
    buffer[i] = '\0';
    return i;
}

extern "C" void _start(char *args)
{
    auto ptr = sbrk(4096);
    if ((long)ptr == EFAILED)
    {
        write(STDERR, "sbrk failed nsh.", 12, 0);
        while (1)
        {
            sleep(100000000);
        }
    }
    char *cmd = (char *)ptr;
    char output_buffer[256];

    while (1)
    {
        write(STDOUT, promot, sizeof(promot), 0);
        int len = read(STDIN, cmd, 4096, 0);
        if (len == 0)
            continue;
        cmd[len] = 0;
        int idx = 0;

        if (idx = start_with(cmd, "cd "); idx != -1)
        {
            if (chdir(cmd + idx) != OK)
            {
                print("unknown dir ");
                print(cmd + idx);
                print("\n");
            }
            continue;
        }
        else if (idx = start_with(cmd, "whereis"); idx == len)
        {
            if (current_dir(output_buffer, sizeof(output_buffer)) != EBUFFER)
            {
                print(output_buffer);
                print("\n");
            }
            else
            {
                print("unknown current dir. systemcall failed\n");
            }
            continue;
        }
        else if (idx = start_with(cmd, "help"); idx == len)
        {
            print(help_msg);
            continue;
        }
        else if (idx = start_with(cmd, "mkdir "); idx != -1)
        {
            if (mkdir(cmd + idx) != OK)
            {
                print("mkdir ");
                print(cmd + idx);
                print("failed. \n");
            }
            continue;
        }
        else if (idx = start_with(cmd, "rmdir "); idx != -1)
        {
            if (rmdir(cmd + idx) != OK)
            {
                print("rmdir ");
                print(cmd + idx);
                print("failed. \n");
            }
            continue;
        }
        auto pid = create_process(cmd, "", 0);
        if (pid < 0)
        {
            write(STDOUT, unknown_command, sizeof(unknown_command), 0);
            print(cmd);
            print("\n");
        }
        else
        {
            long ret;
            wait_process(pid, &ret);
            print("process exit code ");
            itoa(ret, output_buffer, sizeof(output_buffer));
            print(output_buffer);
            print("\n");
        }
    }
}