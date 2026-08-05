#include "kernel/signal.hpp"
#include "naos/abi.h"

int main()
{
    using task::signal::default_action;
    using task::signal::default_action_for;

    if (default_action_for(task::signal::sigstop) != default_action::stop)
        return 1;
    if (default_action_for(task::signal::sigcout) != default_action::continue_process)
        return 2;
    if (default_action_for(task::signal::signone1) != default_action::stop)
        return 3;
    if (default_action_for(task::signal::signone2) != default_action::stop)
        return 4;
    if (default_action_for(task::signal::signone3) != default_action::stop)
        return 5;
    if (default_action_for(task::signal::sigwinch) != default_action::ignore)
        return 6;
    if (na_process_wait_status_exit(0) != 0 || na_process_wait_status_exit(127) != 0x7f00 ||
        na_process_wait_status_exit(-1) != 0xff00)
        return 7;
    return 0;
}
