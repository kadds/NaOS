#include "kernel/arch/idt.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/interrupt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
namespace arch::idt
{

Unpaged_Data_Section ptr_t idt_before_ptr = {0, 0};
ptr_t idt_after_ptr = {0, 0};

const u8 interrupt_type = 0b1110;
const u8 exception_type = 0b1111;

Unpaged_Text_Section void init_before_paging() {}

void init_after_paging()
{

    idt_after_ptr.limit = sizeof(entry) * 128 - 1;
    idt_after_ptr.addr = (u64)memory::NewArray<entry, 16>(memory::KernelBuddyAllocatorV, 128);
    exception::init();
    interrupt::init();
    _load_idt(&idt_after_ptr);
    enable();
}

void enable() { _sti(); }

void disable() { _cli(); }

bool is_enable()
{
    unsigned int IF = 0;
    __asm__ __volatile__("pushf	\n\t"
                         "pop %%rax \n\t"
                         : "=a"(IF)
                         :
                         : "memory");
    return IF & 0x200;
}

void set_entry(int id, void *func, u16 selector, u8 dpl, u8 present, u8 type, u8 ist)
{
    entry *idte = (entry *)idt_after_ptr.addr + id;
    idte->set_offset((u64)func);
    idte->set_selector(selector);
    idte->set_type(type);
    idte->set_dpl(dpl);
    idte->set_valid(present);
    idte->set_ist(ist);
}

void set_exception_entry(int id, void *function, u16 selector, u8 dpl, u8 ist)
{
    set_entry(id, function, selector, dpl, true, exception_type, ist);
}

void set_interrupt_entry(int id, void *function, u16 selector, u8 dpl, u8 ist)
{
    set_entry(id, function, selector, dpl, true, interrupt_type, ist);
}

void set_trap_system_gate(int index, void *func, u8 ist)
{
    idt::set_exception_entry(index, func, gdt::gen_selector(gdt::selector_type::kernel_code, 0), 0, ist);
}

void set_trap_gate(int index, void *func, u8 ist)
{
    idt::set_exception_entry(index, func, gdt::gen_selector(gdt::selector_type::user_code, 3), 3, ist);
}

void set_interrupt_system_gate(int index, void *func, u8 ist)
{
    idt::set_interrupt_entry(index, func, gdt::gen_selector(gdt::selector_type::kernel_code, 0), 0, ist);
}

void set_interrupt_gate(int index, void *func, u8 ist)
{
    idt::set_interrupt_entry(index, func, gdt::gen_selector(gdt::selector_type::user_code, 3), 3, ist);
}
} // namespace arch::idt
