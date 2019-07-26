#include "kernel/gdt.hpp"
#include "kernel/kernel.hpp"
#include "kernel/klib.hpp"
namespace gdt
{

Unpaged_Data_Section descriptor temp_gdt_init[3];
Unpaged_Data_Section ptr_t gdt_before_ptr = {sizeof(temp_gdt_init) - 1, (u64)temp_gdt_init};

descriptor gdt_after_init[] = {0x0, 0x0020980000000000, 0x0000920000000000, 0x0020f80000000000, 0x0000f20000000000, 0,
                               0};
ptr_t gdt_after_ptr = {sizeof(gdt_after_init) - 1, (u64)gdt_after_init};

Unpaged_Text_Section void init_before_paging()
{
    auto *temp_gdt = (u64 *)temp_gdt_init;
    temp_gdt[0] = 0;
    temp_gdt[1] = 0x0020980000000000;
    temp_gdt[2] = 0x0000920000000000;
    _unpaged_load_gdt(&gdt_before_ptr);
    _unpaged_reload_segment(0x08, 0x10);
}
void init_after_paging()
{
    _load_gdt(&gdt_after_ptr);
    _reload_segment(0x08, 0x10);
}
descriptor &get_gdt_descriptor(int index) { return gdt_after_init[index]; }

int get_start_index_of_tss() { return 5; }
::tss::descriptor &get_tss_descriptor(int index)
{
    return *(::tss::descriptor *)&gdt_after_init[index + get_start_index_of_tss()];
}
int get_offset_of_tss() { return get_start_index_of_tss() * sizeof(descriptor); }
} // namespace gdt
