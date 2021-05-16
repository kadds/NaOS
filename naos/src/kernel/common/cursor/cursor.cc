#include "kernel/common/cursor/cursor.hpp"
namespace cursor
{
cursor_t cursor;
int max_x;
int max_y;

void init(int maxx, int maxy)
{
    max_x = maxx;
    max_y = maxy;
    cursor.x = 0;
    cursor.y = 0;
    cursor.state = state_t::hide;
}

void set_cursor(cursor_t c)
{
    cursor = c;
    if (cursor.x > max_x)
    {
        cursor.x = max_x;
    }
    if (cursor.y > max_y)
    {
        cursor.y = max_y;
    }
    if (cursor.x < 0)
    {
        cursor.x = 0;
    }
    if (cursor.y < 0)
    {
        cursor.y = 0;
    }
}

cursor_t get_cursor() { return cursor; }

#define O 0xFF000000
#define G 0x003F3F3F
#define W 0x00FAFAFA

u32 image_normal[] = {
    G, O, O, O, O, O, O, O, O, O, O, O, /* .00000000000 */
    G, G, O, O, O, O, O, O, O, O, O, O, /* ..0000000000 */
    G, W, G, O, O, O, O, O, O, O, O, O, /* .1.000000000 */
    G, W, W, G, O, O, O, O, O, O, O, O, /* .11.00000000 */
    G, W, W, W, G, O, O, O, O, O, O, O, /* .111.0000000 */
    G, W, W, W, W, G, O, O, O, O, O, O, /* .1111.000000 */
    G, W, W, W, W, W, G, O, O, O, O, O, /* .11111.00000 */
    G, W, W, W, W, W, W, G, O, O, O, O, /* .111111.0000 */
    G, W, W, W, W, W, W, W, G, O, O, O, /* .1111111.000 */
    G, W, W, W, W, W, W, W, W, G, O, O, /* .11111111.00 */
    G, W, W, W, W, W, W, W, W, W, G, O, /* .111111111.0 */
    G, W, W, W, W, W, W, G, G, G, G, G, /* .1111111.... */
    G, W, W, G, W, W, G, O, O, O, O, O, /* .11.11.00000 */
    G, W, G, O, G, W, W, G, O, O, O, O, /* .1.0.11.0000 */
    G, G, O, O, G, W, W, G, O, O, O, O, /* ..00.11.0000 */
    G, O, O, O, O, G, W, W, G, O, O, O, /* .0000.11.000 */
    O, O, O, O, O, G, W, W, G, O, O, O, /* 00000.11.000 */
    O, O, O, O, O, O, G, G, G, O, O, O, /* 000000...000 */
};

cursor_image_t get_cursor_image(state_t state)
{
    cursor_image_t image;
    image.offset_x = 0;
    image.offset_y = 0;
    image.data = nullptr;
    image.size_height = 18;
    image.size_width = 12;
    switch (state)
    {
        case state_t::normal:
            image.data = reinterpret_cast<byte *>(image_normal);
            break;
        default:
            break;
    }
    return image;
}

} // namespace cursor