#pragma once

#include <cstdint>

#include <vterm_keycodes.h>

namespace consoled
{
enum class console_key : std::uint64_t
{
    escape = 0x1,
    n1,
    n2,
    n3,
    n4,
    n5,
    n6,
    n7,
    n8,
    n9,
    n0,
    minus,
    equal,
    backspace,
    tab,
    q,
    w,
    e,
    r,
    t,
    y,
    u,
    i,
    o,
    p,
    left_brackets,
    right_brackets,
    enter,
    left_control,
    a,
    s,
    d,
    f,
    g,
    h,
    j,
    k,
    l,
    semicolon,
    quote,
    back_tick,
    left_shift,
    backslash,
    z,
    x,
    c,
    v,
    b,
    n,
    m,
    comma,
    period,
    slash,
    right_shift,
    pad_mul,
    left_alt,
    space,
    capslock,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    numlock,
    scrolllock,
    pad_7,
    pad_8,
    pad_9,
    pad_minus,
    pad_4,
    pad_5,
    pad_6,
    pad_plus,
    pad_1,
    pad_2,
    pad_3,
    pad_0,
    pad_comma,
    f11 = 0x57,
    f12 = 0x58,
    pad_enter = 0x6c,
    right_control,
    mute = 0x70,
    calc,
    play,
    stop = 0x74,
    volume_down = 0x7e,
    volume_up = 0x80,
    pad_slash = 0x85,
    right_alt = 0x88,
    home = 0x97,
    cur_up,
    page_up,
    cur_left,
    cur_right = 0x9d,
    end = 0x9f,
    cur_down,
    page_down,
    insert,
    delete_key,
    print = 0xfd,
    pause = 0xfe,
};

constexpr bool key_to_vterm(console_key key, VTermKey &result) noexcept
{
    switch (key)
    {
        case console_key::enter:
        case console_key::pad_enter:
            result = VTERM_KEY_ENTER;
            return true;
        case console_key::tab:
            result = VTERM_KEY_TAB;
            return true;
        case console_key::backspace:
            result = VTERM_KEY_BACKSPACE;
            return true;
        case console_key::escape:
            result = VTERM_KEY_ESCAPE;
            return true;
        case console_key::cur_up:
            result = VTERM_KEY_UP;
            return true;
        case console_key::cur_down:
            result = VTERM_KEY_DOWN;
            return true;
        case console_key::cur_left:
            result = VTERM_KEY_LEFT;
            return true;
        case console_key::cur_right:
            result = VTERM_KEY_RIGHT;
            return true;
        case console_key::home:
            result = VTERM_KEY_HOME;
            return true;
        case console_key::end:
            result = VTERM_KEY_END;
            return true;
        case console_key::insert:
            result = VTERM_KEY_INS;
            return true;
        case console_key::delete_key:
            result = VTERM_KEY_DEL;
            return true;
        case console_key::page_up:
            result = VTERM_KEY_PAGEUP;
            return true;
        case console_key::page_down:
            result = VTERM_KEY_PAGEDOWN;
            return true;
        default:
            return false;
    }
}
} // namespace consoled
