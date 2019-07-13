#include "kernel/gdt.hpp"
ExportC void _reload_segment(u64 cs, u64 ss);
ExportC void _load_gdt(void *gdt_ptr);

gdt_entry gdt_before_entry[] = {0x0, 0x0020980000000000, 0x0000920000000000};
gdt_ptr gdt_before_ptr = {sizeof(gdt_before_entry) - 1, (u64)gdt_before_entry};

gdt_entry gdt_after_entry[] = {0x0, 0x0020980000000000, 0x0000920000000000};
gdt_ptr gdt_after_ptr = {sizeof(gdt_after_entry) - 1, (u64)gdt_after_entry};

void gdt::init_before_paging()
{
    _load_gdt(&gdt_before_ptr);
    _reload_segment(0x08, 0x10);
}
void gdt::init_after_paging()
{
    _load_gdt(&gdt_after_ptr);
    _reload_segment(0x08, 0x10);
}
