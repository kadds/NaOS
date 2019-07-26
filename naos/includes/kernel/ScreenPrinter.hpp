#pragma once
#include "common.hpp"

class ScreenPrinter
{
  private:
    volatile unsigned char *video = (unsigned char *)0xffff8000000B8000;
    u32 width;
    u32 height;
    int penx;
    int peny;

    void move_pen(int px, int py);
    void move_pen_to_next(char ch);

  public:
    ScreenPrinter(int width, int height)
        : width(width)
        , height(height)
        , penx(0)
        , peny(0){};
    void putchar(char ch);
    void itoa(char *buf, int base, int d);
    void cls();
    void printf(const char *format, ...);
};
extern ScreenPrinter *gPrinter;
