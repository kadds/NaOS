#include "kernel/arch/klib.hpp"
#include "kernel/ksybs.hpp"
#include "kernel/trace.hpp"
void *print_stack(const arch::idt::regs_t *regs, int max_depth)
{
    u64 *rbp;
    u64 *ret = (u64 *)&print_stack;
    u64 *rsp;

    if (regs == nullptr)
    {
        u64 bp, sp;
        __asm__ __volatile__("movq %%rbp, %0 \n\t" : "=r"(bp) : : "memory");
        __asm__ __volatile__("movq %%rsp, %0 \n\t" : "=r"(sp) : : "memory");
        rbp = (u64 *)bp;
        rsp = (u64 *)sp;
    }
    else
    {
        rbp = (u64 *)regs->rbp;
        ret = (u64 *)regs->rip;
        rsp = (u64 *)regs->rsp;
    }
    trace::print(trace::kernel_console_attribute, "stack data(rbp->rsp):");
    u64 least_rsp = (u64)rbp - sizeof(u64) * 10;
    for (u64 i = (u64)rbp; (i >= (u64)rsp || i >= least_rsp) && i >= 0xFFFF800000000000; i -= sizeof(u64))
    {
        trace::print(trace::kernel_console_attribute, "\n    [", (void *)i, "]=", (void *)(*(u64 *)i));
    }

    trace::print(trace::kernel_console_attribute, "\nend of stack data.\n");

    u64 end = (u64)_kernel_text_end;
    u64 start = (u64)_kernel_text_start;
    trace::print(trace::kernel_console_attribute, trace::Background<trace::Color::White>(),
                 trace::Foreground<trace::Color::Red>(), "stack trace: \n", trace::PrintAttr::Reset());
    int i = 0;
    while (rbp != 0)
    {
        if ((u64)rbp < 0xFFFF800000000000)
        {
            break;
        }
        const char *func = ksybs::get_symbol_name(ret);
        if (func != nullptr)
        {
            trace::print(trace::kernel_console_attribute, trace::Foreground<trace::Color::LightCyan>(), func,
                         trace::Foreground<trace::Color::White>(), "[", (void *)ret, "] rbp:", (void *)rbp, "\n");
        }
        else
        {
            trace::print(trace::kernel_console_attribute, trace::Foreground<trace::Color::White>(), (void *)ret,
                         " rbp:", (void *)rbp, "\n");
        }
        ret = (u64 *)*(rbp + 1);
        if ((u64)ret > end || (u64)ret < start)
        {
            break;
        }
        rbp = (u64 *)*rbp;

        if (i++ > max_depth)
            break;
    }
    trace::print(trace::kernel_console_attribute, trace::Background<trace::Color::White>(),
                 trace::Foreground<trace::Color::Red>(), "end of stack trace. \n", trace::PrintAttr::Reset());

    if (regs != nullptr)
    {
        u64 fs, gs;
        __asm__ __volatile__("movq %%fs, %0 \n\t movq %%gs, %1\n\t" : "=r"(fs), "=r"(gs) : : "memory");
        u64 gs_base, k_gs_base, fs_base;
        trace::print(trace::kernel_console_attribute, trace::Foreground<trace::Color::Black>(),
                     trace::Background<trace::Color::LightGray>(), "registers:\n", trace::PrintAttr::Reset(),
                     "rax=", (void *)regs->rax, ", rbx=", (void *)regs->rbx, ", rcx=", (void *)regs->rcx,
                     ", rdx=", (void *)regs->rdx, "\n, r8=", (void *)regs->r8, ", r9=", (void *)regs->r9,
                     ", r10=", (void *)regs->r10, ", r11=", (void *)regs->r11, "\n, r12=", (void *)regs->r12,
                     ", r13=", (void *)regs->r13, ", r14=", (void *)regs->r14, ", r15=", (void *)regs->r15,
                     "\n, rdi=", (void *)regs->rdi, ", rsi=", (void *)regs->rsi, "\ncs=", (void *)regs->cs,
                     ", ds=", (void *)regs->ds, ", es=", (void *)regs->es, ", fs=", (void *)fs, ", gs=", (void *)gs,
                     ",ss=", (void *)regs->ss, "\nrip=", (void *)regs->rip, ", rsp=", (void *)regs->rsp,
                     ", rbp=", (void *)regs->rbp, "\nrflags=", (void *)regs->rflags, "\nvector=", (void *)regs->vector,
                     ", error_code=", (void *)regs->error_code, "\n");

        k_gs_base = _rdmsr(0xC0000102);
        gs_base = _rdmsr(0xC0000101);
        fs_base = _rdmsr(0xC0000100);
        trace::print(trace::kernel_console_attribute, "current kernel_gs_base=", (void *)k_gs_base,
                     ", gs_base=", (void *)gs_base, ", fs_base=", (void *)fs_base, '\n');

        trace::print(trace::kernel_console_attribute, trace::Foreground<trace::Color::Black>(),
                     trace::Background<trace::Color::LightGray>(), "end of registers.\n", trace::PrintAttr::Reset());
    }
    return (void *)rbp;
}
