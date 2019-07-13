#include "loader/ScreenPrinter.hpp"
#include <stdarg.h>
ScreenPrinter *gPrinter;

void ScreenPrinter::cls()
{
    for (u32 i = 0; i < width * height * 2; i++)
        *(video + i) = 0;
    penx = 0;
    peny = 0;
}

void ScreenPrinter::itoa(char *buf, int base, int d)
{
    char *p = buf;
    char *p1, *p2;
    unsigned long ud = d;
    int divisor = 10;

    if (base == 'd' && d < 0)
    {
        *p++ = '-';
        buf++;
        ud = -d;
    }
    else if (base == 'x')
        divisor = 16;
    do
    {
        int remainder = ud % divisor;
        *p++ = (remainder < 10) ? remainder + '0' : remainder + 'a' - 10;
    } while (ud /= divisor);

    // \0
    *p = 0;
    p1 = buf;
    p2 = p - 1;
    while (p1 < p2)
    {
        char tmp = *p1;
        *p1 = *p2;
        *p2 = tmp;
        p1++;
        p2--;
    }
}
void ScreenPrinter::move_pen(int px, int py)
{
    if (py >= (int)height)
        py = 0;

    penx = px;
    peny = py;
}
void ScreenPrinter::move_pen_to_next(char ch)
{
    if (ch == '\n' || ch == '\r' || (penx > (int)width - 1))
    {
        move_pen(0, peny + 1);
        return;
    }
    move_pen(penx + 1, peny);
}
void ScreenPrinter::putchar(char ch)
{
    if (ch != '\t' && ch != '\n')
    {
        *(video + (penx + peny * width) * 2) = ch;
        *(video + (penx + peny * width) * 2 + 1) = 0x7;
    }
    move_pen_to_next(ch);
}

void ScreenPrinter::printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);

    char c;
    char buf[20];

    while ((c = *format++) != 0)
    {
        if (c != '%')
            putchar(c);
        else
        {
            const char *p, *p2;
            int pad0 = 0, pad = 0;

            c = *format++;
            if (c == '0')
            {
                pad0 = 1;
                c = *format++;
            }

            if (c >= '0' && c <= '9')
            {
                pad = c - '0';
                c = *format++;
            }

            switch (c)
            {
            case 'd':
            case 'u':
            case 'x':
                itoa(buf, c, va_arg(ap, int));
                p = buf;
                goto string;
                break;

            case 's':
                p = va_arg(ap, char *);
                if (!p)
                    p = "(null)";

            string:
                for (p2 = p; *p2; p2++)
                    ;
                for (; p2 < p + pad; p2++)
                    putchar(pad0 ? '0' : ' ');
                while (*p)
                    putchar(*p++);
                break;

            default:
                putchar(va_arg(ap, int));
                break;
            }
        }
    }

    va_end(ap);
}