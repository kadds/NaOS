#include "kernel/arch/klib.hpp"
#include "kernel/ksybs.hpp"
#include "kernel/trace.hpp"
void *print_stack(const regs_t *regs, int max_depth)
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
    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("stack data(rbp->rsp):");
    trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();

    u64 least_rsp = (u64)rbp - sizeof(u64) * 10;
    u32 p = 0;
    for (u64 i = (u64)rbp; (i >= (u64)rsp || i >= least_rsp) && i >= 0xFFFF800000000000 && p < 100;
         i -= sizeof(u64), p++)
    {
        /// print stack value
        trace::print("\n    [", (void *)i, "]=", (void *)(*(u64 *)i));
    }

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("\nend of stack data.\n");

    u64 end = (u64)_file_end + (u64)base_virtual_addr;
    u64 start = (u64)_file_start + (u64)base_virtual_addr;
    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("stack trace:\n");
    int i = 0;
    while ((u64)rbp != 0)
    {
        if (((u64)rbp) < 0xFFFF800000000000)
        {
            break;
        }
        const char *func = ksybs::get_symbol_name(ret);
        if (func != nullptr)
        {
            trace::print<trace::PrintAttribute<trace::CFG::LightCyan>>("[", (void *)ret, "]");
            trace::print<trace::PrintAttribute<trace::CBK::White>>("rbp:", (void *)rbp, "\n");
        }
        else
        {
            trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Default>>((void *)ret,
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
    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("end of stack trace. \n");

    if (regs != nullptr)
    {
        u64 fs, gs;
        __asm__ __volatile__("movq %%fs, %0 \n\t movq %%gs, %1\n\t" : "=r"(fs), "=r"(gs) : : "memory");
        trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("registers(with intr regs):");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
        trace::print("\nrax=", (void *)regs->rax, ", rbx=", (void *)regs->rbx, ", rcx=", (void *)regs->rcx,
                     ", rdx=", (void *)regs->rdx, ", r8=", (void *)regs->r8, ", r9=", (void *)regs->r9,
                     ", r10=", (void *)regs->r10, ", r11=", (void *)regs->r11, ", r12=", (void *)regs->r12,
                     ", r13=", (void *)regs->r13, ", r14=", (void *)regs->r14, ", r15=", (void *)regs->r15,
                     ", rdi=", (void *)regs->rdi, ", rsi=", (void *)regs->rsi, ", cs=", (void *)regs->cs,
                     ", ds=", (void *)regs->ds, ", es=", (void *)regs->es, ", fs=", (void *)fs, ", gs=", (void *)gs,
                     ", ss=", (void *)regs->ss, ", rip=", (void *)regs->rip, ", rsp=", (void *)regs->rsp,
                     ", rbp=", (void *)regs->rbp, ", rflags=", (void *)regs->rflags, ", vector=", (void *)regs->vector,
                     ", error_code=", (void *)regs->error_code, "\n");
    }
    else
    {
        u64 cs, ds, es, fs, gs, ss;
        __asm__ __volatile__("movq %%fs, %0 \n\t movq %%gs, %1\n\t  movq %%ds, %2\n\t  movq %%es,  %3\n\t  movq %%ss, "
                             "%4\n\t movq %%cs, %5\n\t"
                             : "=r"(fs), "=r"(gs), "=r"(ds), "=r"(es), "=r"(ss), "=r"(cs)
                             :
                             : "memory");
        u64 rsp, rip, rbp, rflags;
        __asm__ __volatile__("movq %%rsp, %0 \n\t lea (%%rip), %1\n\t movq %%rbp, %2 \n\t pushf \n\t popq %3"
                             : "=r"(rsp), "=r"(rip), "=r"(rbp), "=r"(rflags)
                             :
                             : "memory");

        trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("registers(without intr regs):");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
        trace::print<>("\ncs=", (void *)cs, ", ds=", (void *)ds, ", es=", (void *)es, ", fs=", (void *)fs,
                       ", gs=", (void *)gs, ", ss=", (void *)ss, ", rip=", (void *)rip, ", rsp=", (void *)rsp,
                       ", rbp=", (void *)rbp, ", rflags=", (void *)rflags, "\n");
    }
    u64 gs_base, k_gs_base, fs_base;

    k_gs_base = _rdmsr(0xC0000102);
    gs_base = _rdmsr(0xC0000101);
    fs_base = _rdmsr(0xC0000100);
    trace::print("current kernel_gs_base=", (void *)k_gs_base, ", gs_base=", (void *)gs_base,
                 ", fs_base=", (void *)fs_base, '\n');
    u64 cr0, cr2, cr3, cr4, cr8;
    __asm__ __volatile__(
        "movq %%cr0, %0 \n\t movq %%cr2, %1\n\t  movq %%cr3, %2\n\t  movq %%cr4,  %3\n\t  movq %%cr8, %4\n\t "
        : "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4), "=r"(cr8)
        :
        : "memory");

    trace::print("current control registers: cr0=", (void *)cr0, ", cr2=", (void *)cr2, ", cr3=", (void *)cr3,
                 ", cr4=", (void *)cr4, ", cr8=", (void *)cr8, '\n');

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("end of registers.", '\n');
    trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
    return (void *)rbp;
}
