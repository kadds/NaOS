#include "kernel/input/terminal_shortcut.hpp"

#include "catch2_compat.hpp"

TEST_CASE("terminal shortcuts require control")
{
    REQUIRE(input::terminal_shortcut_target(input::key::f1, false, false) == input::terminal_target::none);
    REQUIRE(input::terminal_shortcut_target(input::key::f12, false, true) == input::terminal_target::none);
}

TEST_CASE("control function keys select terminal")
{
    REQUIRE(input::terminal_shortcut_target(input::key::f1, true, false) == input::terminal_target::user);
    REQUIRE(input::terminal_shortcut_target(input::key::f12, true, false) == input::terminal_target::kernel);
    REQUIRE(input::terminal_shortcut_target(input::key::f1, true, true) == input::terminal_target::user);
    REQUIRE(input::terminal_shortcut_target(input::key::f12, true, true) == input::terminal_target::kernel);
    REQUIRE(input::terminal_shortcut_target(input::key::f2, true, false) == input::terminal_target::none);
}
