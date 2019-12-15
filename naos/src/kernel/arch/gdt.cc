#include "kernel/arch/gdt.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/kernel.hpp"
namespace arch::gdt
{

Unpaged_Data_Section Aligned(8) descriptor temp_gdt_init[3];
Unpaged_Data_Section ptr_t gdt_before_ptr = {sizeof(temp_gdt_init) - 1, (u64)temp_gdt_init};

Aligned(8) descriptor gdt_after_init[cpu::max_cpu_support * 2 + 10] = {
    0,                  // null
    0,                  // null
    0x0000920000000000, // kernel data
    0x0020980000000000, // kernel code
    0x0000920000000000, // kernel data,
    0,
    0x0000f20000000000, // user data
    0x0020f80000000000, // user code
    0x0000f20000000000, // user data
    0,
    0, // tss start
};

ptr_t gdt_after_ptr = {sizeof(gdt_after_init) - 1, (u64)gdt_after_init};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
Unpaged_Text_Section void init_before_paging(bool is_bsp)
{
    if (is_bsp)
    {
        auto *temp_gdt = (u64 *)temp_gdt_init;
        temp_gdt[0] = 0;
        temp_gdt[1] = 0x0020980000000000;
        temp_gdt[2] = 0x0000920000000000;
        __asm__ __volatile__("lgdt	(%0)\n\t" : : "r"(&gdt_before_ptr) : "memory");
        _unpaged_reload_segment(0x08, 0x10);
    }
}
#pragma GCC diagnostic pop

void init_after_paging()
{
    __asm__ __volatile__("lgdt	(%0)\n\t" : : "r"(&gdt_after_ptr) : "memory");
    _reload_segment(gen_selector(selector_type::kernel_code, 0), gen_selector(selector_type::kernel_data, 0));
}

descriptor &get_gdt_descriptor(int index) { return gdt_after_init[index]; }

int get_start_index_of_tss() { return 10; }

::arch::tss::descriptor &get_tss_descriptor(int index)
{
    return *(::arch::tss::descriptor *)&gdt_after_init[index * 2 + get_start_index_of_tss()];
}

int get_offset_of_tss() { return get_start_index_of_tss() * sizeof(descriptor); }
} // namespace arch::gdt
