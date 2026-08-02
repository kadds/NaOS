#pragma once

#include "common.hpp"
#include <cstddef>

namespace dev::tty
{
/// Kernel-side copy of the NaOS mlibc termios ABI.
struct termios_t
{
    u32 c_iflag;
    u32 c_oflag;
    u32 c_cflag;
    u32 c_lflag;
    u8 c_line;
    u8 c_cc[32];
    u32 ibaud;
    u32 obaud;
};

static_assert(offsetof(termios_t, c_cc) == 17);
static_assert(sizeof(termios_t) == 60);

struct winsize_t
{
    u16 ws_row;
    u16 ws_col;
    u16 ws_xpixel;
    u16 ws_ypixel;
};

static_assert(sizeof(winsize_t) == 8);

namespace termios_cc
{
inline constexpr u8 vintr = 0;
inline constexpr u8 vquit = 1;
inline constexpr u8 verase = 2;
inline constexpr u8 vkill = 3;
inline constexpr u8 veof = 4;
inline constexpr u8 vtime = 5;
inline constexpr u8 vmin = 6;
inline constexpr u8 vsusp = 10;
} // namespace termios_cc

namespace termios_iflag
{
inline constexpr u32 ignbrk = 0000001;
inline constexpr u32 brkint = 0000002;
inline constexpr u32 ignpar = 0000004;
inline constexpr u32 parmrk = 0000010;
inline constexpr u32 inpck = 0000020;
inline constexpr u32 istrip = 0000040;
inline constexpr u32 inlcr = 0000100;
inline constexpr u32 igncr = 0000200;
inline constexpr u32 icrnl = 0000400;
inline constexpr u32 ixon = 0002000;
inline constexpr u32 ixany = 0004000;
inline constexpr u32 ixoff = 0010000;
} // namespace termios_iflag

namespace termios_oflag
{
inline constexpr u32 opost = 0000001;
inline constexpr u32 onlcr = 0000004;
inline constexpr u32 ocrnl = 0000010;
inline constexpr u32 onocr = 0000020;
inline constexpr u32 onlret = 0000040;
inline constexpr u32 ofill = 0000100;
inline constexpr u32 ofdel = 0000200;
} // namespace termios_oflag

namespace termios_cflag
{
inline constexpr u32 cs8 = 0000060;
inline constexpr u32 cread = 0000200;
} // namespace termios_cflag

namespace termios_lflag
{
inline constexpr u32 isig = 0000001;
inline constexpr u32 icanon = 0000002;
inline constexpr u32 echo = 0000010;
inline constexpr u32 echoe = 0000020;
inline constexpr u32 echok = 0000040;
inline constexpr u32 echonl = 0000100;
inline constexpr u32 noflsh = 0000200;
inline constexpr u32 tostop = 0000400;
inline constexpr u32 ixten = 0100000;
} // namespace termios_lflag

namespace tty_ioctl
{
// Linux-compatible values used by the NaOS mlibc ABI.
inline constexpr u64 tcgets = 0x5401;
inline constexpr u64 tcsets = 0x5402;
inline constexpr u64 tcsetsw = 0x5403;
inline constexpr u64 tcsetsf = 0x5404;
inline constexpr u64 tcflsh = 0x540B;
inline constexpr u64 tiocsctty = 0x540E;
inline constexpr u64 tiocgpgrp = 0x540F;
inline constexpr u64 tiocspgrp = 0x5410;
inline constexpr u64 tiocoutq = 0x5411;
inline constexpr u64 tiocgwinsz = 0x5413;
inline constexpr u64 tiocswinsz = 0x5414;
inline constexpr u64 tiocnotty = 0x5422;
inline constexpr u64 tiocgsid = 0x5429;
inline constexpr u64 tiocgptn = 0x80045430;
inline constexpr u64 tiocsptlck = 0x40045431;
inline constexpr u64 tiocgptlck = 0x80045439;
} // namespace tty_ioctl

namespace tty_flush
{
inline constexpr i32 input = 0;
inline constexpr i32 output = 1;
inline constexpr i32 both = 2;
} // namespace tty_flush

} // namespace dev::tty
