#include "kernel/signal.hpp"
#include "naos/abi.h"

#include "catch2_compat.hpp"

TEST_CASE("signal default policy", "[signal]")
{
    using task::signal::default_action;
    using task::signal::default_action_for;

    REQUIRE(default_action_for(task::signal::sigstop) == default_action::stop);
    REQUIRE(default_action_for(task::signal::sigcout) == default_action::continue_process);
    REQUIRE(default_action_for(task::signal::signone1) == default_action::stop);
    REQUIRE(default_action_for(task::signal::signone2) == default_action::stop);
    REQUIRE(default_action_for(task::signal::signone3) == default_action::stop);
    REQUIRE(default_action_for(task::signal::sigwinch) == default_action::ignore);
    REQUIRE(na_process_wait_status_exit(0) == 0);
    REQUIRE(na_process_wait_status_exit(127) == 0x7f00);
    REQUIRE(na_process_wait_status_exit(-1) == 0xff00);
}
