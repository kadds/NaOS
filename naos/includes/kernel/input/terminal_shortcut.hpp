#pragma once

#include "kernel/input/key.hpp"

namespace input
{
enum class terminal_target : u8
{
    none,
    user,
    kernel,
};

// Accept both Ctrl+Fn and Ctrl+Alt+Fn so the shortcut works through QEMU GTK
// and common desktop keyboard configurations.
constexpr terminal_target terminal_shortcut_target(key key_code, bool control, bool alt) noexcept
{
    (void)alt;
    if (!control)
        return terminal_target::none;
    if (key_code == key::f1)
        return terminal_target::user;
    if (key_code == key::f12)
        return terminal_target::kernel;
    return terminal_target::none;
}
} // namespace input
