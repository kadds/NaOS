#include "kernel/idt.hpp"
#include "kernel/exception.hpp"
#include "kernel/interrupt.hpp"
#include "kernel/kernel.hpp"
#include "kernel/memory.hpp"
namespace idt
{

Unpaged_Data_Section ptr_t idt_before_ptr = {0, 0};
ptr_t idt_after_ptr = {0, 0};
ExportC void _load_idt(void *idt_addr);
ExportC void _cli();
ExportC void _sti();
const u8 interrupt_type = 0b1110;
const u8 exception_type = 0b1111;

Unpaged_Text_Section void init_before_paging() {}
void init_after_paging()
{
    idt_after_ptr.limit = 255;
    idt_after_ptr.addr = (u64)memory::alloc(sizeof(entry) * 256, 8);

    exception::init();
    interrupt::init();

    _load_idt(&idt_after_ptr);
    // enable();
}
void enable() { _sti(); }

void disable() { _cli(); }
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
} // namespace idt
