#include "kernel/arch/gdt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/kernel.hpp"
namespace arch::gdt
{

Unpaged_Data_Section Aligned(8) descriptor temp_gdt_init[3];
Unpaged_Data_Section ptr_t gdt_before_ptr = {sizeof(temp_gdt_init) - 1, (u64)temp_gdt_init};

Aligned(8) descriptor gdt_after_init[] = {
    0x0, 0x0020980000000000, 0x0000920000000000, 0x0020f80000000000, 0x0000f20000000000, 0, 0};

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
    _reload_segment(gen_selector(selector_type::kernel_code, 0), gen_selector(selector_type::kernel_data, 0));
}
descriptor &get_gdt_descriptor(int index) { return gdt_after_init[index]; }

int get_start_index_of_tss() { return 5; }
::arch::tss::descriptor &get_tss_descriptor(int index)
{
    return *(::arch::tss::descriptor *)&gdt_after_init[index + get_start_index_of_tss()];
}
int get_offset_of_tss() { return get_start_index_of_tss() * sizeof(descriptor); }
} // namespace arch::gdt
