#include "kernel/idt.hpp"

idt_ptr idt_before_ptr = {0, 0};

void idt::init_before_paging() {}
void idt::init_after_paging() {}
