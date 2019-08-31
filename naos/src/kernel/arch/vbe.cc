#include "kernel/arch/vbe.hpp"
#include "kernel/mm/memory.hpp"
#include <stdarg.h>
namespace arch::device::vbe
{

volatile unsigned char *video = (unsigned char *)0xB8000;
u32 width;
u32 height;
int penx;
int peny;
void init()
{
    width = 80;
    height = 25;
    penx = 0;
    peny = 0;
    cls();
}
void mm_addr() { video = memory::kernel_phyaddr_to_virtaddr(video); }
void cls()
{
    for (u32 i = 0; i < width * height * 2; i++)
        *(video + i) = 0;
    penx = 0;
    peny = 0;
}

void move_pen(int px, int py)
{
    if (py >= (int)height)
        py = 0;

    penx = px;
    peny = py;
}
void move_pen_to_next(char ch)
{
    if (ch == '\n' || ch == '\r' || (penx > (int)width - 1))
    {
        move_pen(0, peny + 1);
        return;
    }
    move_pen(penx + 1, peny);
}
void putchar(char ch)
{
    if (ch != '\t' && ch != '\n')
    {
        *(video + (penx + peny * width) * 2) = ch;
        *(video + (penx + peny * width) * 2 + 1) = 0x7;
    }
    move_pen_to_next(ch);
}
void putstring(const char *str)
{
    while (1)
    {
        putchar(*str++);
        if (*str == '\0')
            return;
    }
}

} // namespace arch::device::vbe
