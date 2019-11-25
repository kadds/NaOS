#pragma once
#include "common.hpp"
namespace trace
{
struct console_attribute;
};
namespace arch::device::vga
{

struct cursor_t
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

u64 putstring(const char *str, u64 max_len, const trace::console_attribute &attribute);
void tag_memory();

} // namespace arch::device::vga
