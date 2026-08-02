#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

void *__dso_handle;

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCSBRK 0x5409
#define TCXONC 0x540A
#define TCFLSH 0x540B
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGSID 0x5429
#define TCSBRKP 0x5425
#define TIOCGPTN 0x80045430
#define TIOCSPTLCK 0x40045431

extern void *memcpy(void *destination, const void *source, size_t size);
extern int *__errno_location(void);

void *mempcpy(void *destination, const void *source, size_t size)
{
    return (char *)memcpy(destination, source, size) + size;
}

enum ioctl_argument_kind
{
    ioctl_argument_none,
    ioctl_argument_pointer,
    ioctl_argument_integer,
};

static enum ioctl_argument_kind get_ioctl_argument_kind(unsigned long request)
{
    switch (request)
    {
        case TCGETS:
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
        case TIOCGPGRP:
        case TIOCSPGRP:
        case TIOCGWINSZ:
        case TIOCSWINSZ:
        case TIOCGSID:
        case TIOCGPTN:
        case TIOCSPTLCK:
            return ioctl_argument_pointer;
        case TCSBRK:
        case TCXONC:
        case TCFLSH:
        case TCSBRKP:
        case TIOCSCTTY:
            return ioctl_argument_integer;
        case 0x4600: // FBIOGET_VSCREENINFO
        case 0x4601: // FBIOPUT_VSCREENINFO
        case 0x4602: // FBIOGET_FSCREENINFO
        case 0x460f: // FBIOGET_CON2FBMAP
        case 0x4610: // FBIOPUT_CON2FBMAP
        case 0x4620: // NaOS FBIOGET_ACTIVE_TERMINAL
            return ioctl_argument_pointer;
        case 0x4621: // NaOS FBIOSET_ACTIVE_TERMINAL
            return ioctl_argument_integer;
        case 0x4611: // FBIOBLANK takes an integer blanking level
            return ioctl_argument_integer;
        default:
            // For encoded _IO commands, no argument is part of the ABI. Legacy
            // commands with an argument are listed explicitly above.
            return ((request >> 16) & 0x3fff) == 0 && ((request >> 30) & 0x3) == 0
                       ? ioctl_argument_none
                       : ioctl_argument_pointer;
    }
}

int ioctl(int fd, unsigned long request, ...)
{
    void *argument = NULL;
    const enum ioctl_argument_kind kind = get_ioctl_argument_kind(request);
    if (kind != ioctl_argument_none)
    {
        va_list args;
        va_start(args, request);
        if (kind == ioctl_argument_integer)
        {
            const int value = va_arg(args, int);
            argument = (void *)(uintptr_t)(unsigned int)value;
        }
        else
        {
            argument = va_arg(args, void *);
        }
        va_end(args);
    }

    long result;
    __asm__ volatile("movq $67, %%rax\n\tsyscall"
                     : "=a"(result)
                     : "D"((long)fd), "S"(request), "d"(argument)
                     : "rcx", "r11", "memory");
    if (result == 0)
    {
        return 0;
    }

    *__errno_location() = result < 0 ? -result : result;
    return -1;
}
