#include "../common/arch/lib.hpp"
const char hello[] = "userland init\e[32;44mhi,\e[0minit! \e[1;38;2;255;0;25;49mcolor text\e[0m\n";
const char shell_failed[] = "execute nsh failed, check rootfs image and memory size in system.\nhlt at init.\n";
const char startup_shell[] = "startup nsh...\n";

long nsh_pid = 0;

void signal_event()
{
    int num;
    sig_info_t info;
    while (1)
    {
        sigwait(&num, &info);
        sigtarget_t target;
        target.id = nsh_pid;
        target.flags = SIGTGT_PROC;
        if (target.id)
            sigsend(&target, SIGINT, nullptr);
    }
}

extern "C" void _start(int argc, char **argv)
{
    write(STDOUT, hello, sizeof(hello), 0);
    write(STDOUT, startup_shell, sizeof(startup_shell), 0);
    { /// set signal
        sig_mask_t mask;
        sig_mask_init(mask);
        sig_mask_set(mask, SIGINT);

        sigmask(SIGOPT_OR, &mask, nullptr, nullptr);
        auto sig_id = create_thread((void *)signal_event, 0, 0);
        detach(sig_id);
    }

    // run shell
    while (1)
    {
        nsh_pid = create_process("/bin/nsh", nullptr, 0);
        if (nsh_pid <= 0)
        {
            write(STDOUT, shell_failed, sizeof(shell_failed), 0);
            while (1)
            {
                sleep(1000000000);
            }
        }
        wait_process(nsh_pid, nullptr);
    }
}