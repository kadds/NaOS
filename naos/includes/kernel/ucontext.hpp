#pragma once
#include "kernel/arch/idt.hpp"
namespace uctx
{

struct UnInterruptableContext
{
    bool IF;
    UnInterruptableContext()
    {
        IF = arch::idt::is_enable();
        arch::idt::disable();
    }
    ~UnInterruptableContext()
    {
        if (IF)
        {
            arch::idt::enable();
        }
        else
        {
            arch::idt::disable();
        }
    }
};
} // namespace uctx
