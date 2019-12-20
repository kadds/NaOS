#pragma once
#include "common.hpp"
namespace input
{
enum class key : u8
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
    semicolon, // ;
    quote,     // '
    back_tick, // `
    left_shift,
    backslash, // it is'\'
    z,
    x,
    c,
    v,
    b,
    n,
    m,
    comma,  // ,
    period, // .
    slash,  // /
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

    //=====
    f11 = 0x57,
    f12 = 0x58,

    pad_enter = 0x6C,
    right_control,

    mute = 0x70,
    calc,
    play,
    stop = 0x74,

    volume_down = 0x7E,
    volume_up = 0x80,

    pad_slash = 0x85,
    right_alt = 0x88,
    home = 0x97,
    cur_up,
    page_up,
    cur_left,

    cur_right = 0x9D,

    end = 0x9F,
    cur_down,
    page_down,
    insert,
    delete_key,

    //=====

    print = 0xfd,
    pause = 0xfe,

};
} // namespace input
