#pragma once
#include "common.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/framebuffer.hpp"
#include "kernel/terminal.hpp"
namespace trace
{
struct console_attribute;
}

namespace arch::device::vga
{
term::minimal_terminal *early_init(fb::framebuffer_t fb);
void init();

} // namespace arch::device::vga
