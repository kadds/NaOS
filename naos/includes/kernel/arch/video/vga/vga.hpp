#pragma once
#include "common.hpp"
namespace trace
{
struct console_attribute;
}
namespace arch::device::vga
{

struct text_cursor_t
{
    u64 px;
    u64 py;
};
extern bool is_auto_flush;

void init();

byte *get_vram();
void set_vram(byte *addr);

byte *get_backbuffer();
void set_backbuffer(byte *addr);

void auto_flush();
void flush();
void flush_kbuffer();

u64 putstring(const char *str, u64 max_len);
void tag_memory();

} // namespace arch::device::vga
