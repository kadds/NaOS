#pragma once
#include "common.hpp"
namespace cursor
{
enum class state_t
{
    normal = 0,
    hide = 1,
};

struct cursor_t
{
    int x;
    int y;
    state_t state;
    bool operator==(const cursor_t &rhs) const { return x == rhs.x && y == rhs.y && state == rhs.state; }
    bool operator!=(const cursor_t &rhs) const { return operator==(rhs); }
};

struct cursor_image_t
{
    byte *data;
    int size_height;
    int size_width;
    int offset_x;
    int offset_y;
};

void init(int maxx, int maxy);

void set_cursor(cursor_t cursor);
cursor_t get_cursor();

cursor_image_t get_cursor_image(state_t state);

} // namespace cursor
