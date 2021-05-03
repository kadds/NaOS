#include "kernel/arch/klib.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/ksybs.hpp"
#include "kernel/trace.hpp"

void *print_stack(const regs_t *regs, int max_depth)
{
    u64 rbp;
    u64 ret = reinterpret_cast<u64>(&print_stack);
    u64 rsp;

    if (regs == nullptr)
    {
        u64 bp, sp;
        __asm__ __volatile__("movq %%rbp, %0 \n\t" : "=g"(bp) : :);
        __asm__ __volatile__("movq %%rsp, %0 \n\t" : "=g"(sp) : :);
        rbp = bp;
        rsp = sp;
    }
    else
    {
        rbp = regs->rbp;
        ret = regs->rip;
        rsp = regs->rsp;
    }
    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("stack data(rbp->rsp):");
    trace::print_reset();

    u64 least_rsp = rbp - sizeof(u64) * 10;
    u32 p = 0;
    for (u64 i = rbp; (i >= rsp || i >= least_rsp) && is_kernel_space_pointer(i) && p < 100; i -= sizeof(u64), p++)
    {
        /// print stack value
        u64 *val_of_i = reinterpret_cast<u64 *>(i);
        trace::print("\n    [", reinterpret_cast<addr_t>(i), "]=", reinterpret_cast<addr_t>(*val_of_i));
    }

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("\nend of stack data.\n");

    u64 end = (u64)_file_end + (u64)base_virtual_addr;
    u64 start = (u64)_file_start + (u64)base_virtual_addr;
    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("stack trace:\n");
    trace::print<trace::PrintAttribute<trace::CBK::Default>>();
    int i = 0;
    while ((u64)rbp != 0)
    {
        if (!is_kernel_space_pointer(rbp))
        {
            break;
        }
        const char *func = ksybs::get_symbol_name(reinterpret_cast<addr_t>(ret));
        if (func != nullptr)
        {
            trace::print<trace::PrintAttribute<trace::CFG::LightCyan>>(func);
            trace::print<trace::PrintAttribute<trace::CFG::Default>>(" [", reinterpret_cast<addr_t>(ret), "]");
            trace::print<trace::PrintAttribute<trace::CFG::White>>(" rbp:", reinterpret_cast<addr_t>(rbp), "\n");
        }
        else
        {
            trace::print<trace::PrintAttribute<trace::CBK::Default, trace::CFG::Default>>(
                reinterpret_cast<addr_t>(ret), " rbp:", reinterpret_cast<addr_t>(rbp), "\n");
        }
        ret = *(reinterpret_cast<u64 *>(rbp) + 1);
        if (ret > end || ret < start)
        {
            break;
        }
        rbp = reinterpret_cast<u64>(*reinterpret_cast<u64 *>(rbp));

        if (i++ > max_depth)
            break;
    }

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("end of stack trace. \n");

    if (regs != nullptr)
    {
        u64 fs, gs;
        __asm__ __volatile__("movq %%fs, %0 \n\t movq %%gs, %1\n\t" : "=r"(fs), "=r"(gs) : :);
        trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("registers(with intr regs):");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
        trace::print("\nrax=", reinterpret_cast<addr_t>(regs->rax), ", rbx=", reinterpret_cast<addr_t>(regs->rbx),
                     ", rcx=", reinterpret_cast<addr_t>(regs->rcx), ", rdx=", reinterpret_cast<addr_t>(regs->rdx),
                     ", r8=", reinterpret_cast<addr_t>(regs->r8), ", r9=", reinterpret_cast<addr_t>(regs->r9),
                     ", r10=", reinterpret_cast<addr_t>(regs->r10), ", r11=", reinterpret_cast<addr_t>(regs->r11),
                     ", r12=", reinterpret_cast<addr_t>(regs->r12), ", r13=", reinterpret_cast<addr_t>(regs->r13),
                     ", r14=", reinterpret_cast<addr_t>(regs->r14), ", r15=", reinterpret_cast<addr_t>(regs->r15),
                     ", rdi=", reinterpret_cast<addr_t>(regs->rdi), ", rsi=", reinterpret_cast<addr_t>(regs->rsi),
                     ", cs=", reinterpret_cast<addr_t>(regs->cs), ", ds=", reinterpret_cast<addr_t>(regs->ds),
                     ", es=", reinterpret_cast<addr_t>(regs->es), ", fs=", reinterpret_cast<addr_t>(fs),
                     ", gs=", reinterpret_cast<addr_t>(gs), ", ss=", reinterpret_cast<addr_t>(regs->ss),
                     ", rip=", reinterpret_cast<addr_t>(regs->rip), ", rsp=", reinterpret_cast<addr_t>(regs->rsp),
                     ", rbp=", reinterpret_cast<addr_t>(regs->rbp), ", rflags=", reinterpret_cast<addr_t>(regs->rflags),
                     ", vector=", reinterpret_cast<addr_t>(regs->vector),
                     ", error_code=", reinterpret_cast<addr_t>(regs->error_code), "\n");
    }
    else
    {
        u64 cs, ds, es, fs, gs, ss;
        __asm__ __volatile__("movq %%fs, %0 \n\t movq %%gs, %1\n\t  movq %%ds, %2\n\t  movq %%es,  %3\n\t  movq %%ss, "
                             "%4\n\t movq %%cs, %5\n\t"
                             : "=r"(fs), "=r"(gs), "=r"(ds), "=r"(es), "=r"(ss), "=r"(cs)
                             :
                             :);
        u64 reg_rsp, reg_rip, reg_rbp, reg_rflags;
        __asm__ __volatile__("movq %%rsp, %0 \n\t leaq (%%rip), %1\n\t movq %%rbp, %2 \n\t pushf \n\t popq %3"
                             : "=g"(reg_rsp), "=r"(reg_rip), "=g"(reg_rbp), "=g"(reg_rflags)
                             :
                             :);

        trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("registers(without intr regs):");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
        trace::print<>("\ncs=", reinterpret_cast<addr_t>(cs), ", ds=", reinterpret_cast<addr_t>(ds),
                       ", es=", reinterpret_cast<addr_t>(es), ", fs=", reinterpret_cast<addr_t>(fs),
                       ", gs=", reinterpret_cast<addr_t>(gs), ", ss=", reinterpret_cast<addr_t>(ss),
                       ", rip=", reinterpret_cast<addr_t>(reg_rip), ", rsp=", reinterpret_cast<addr_t>(reg_rsp),
                       ", rbp=", reinterpret_cast<addr_t>(reg_rbp), ", rflags=", reinterpret_cast<addr_t>(reg_rflags),
                       "\n");
    }
    u64 gs_base, k_gs_base, fs_base;

    k_gs_base = _rdmsr(0xC0000102);
    gs_base = _rdmsr(0xC0000101);
    fs_base = _rdmsr(0xC0000100);
    trace::print("msr(now): kernel_gs_base=", reinterpret_cast<addr_t>(k_gs_base),
                 ", gs_base=", reinterpret_cast<addr_t>(gs_base), ", fs_base=", reinterpret_cast<addr_t>(fs_base),
                 '\n');
    u64 cr0, cr2, cr3, cr4, cr8;
    __asm__ __volatile__(
        "movq %%cr0, %0 \n\t movq %%cr2, %1\n\t  movq %%cr3, %2\n\t  movq %%cr4,  %3\n\t  movq %%cr8, %4\n\t "
        : "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4), "=r"(cr8)
        :
        :);

    trace::print("control registers(now): cr0=", reinterpret_cast<addr_t>(cr0), ", cr2=", reinterpret_cast<addr_t>(cr2),
                 ", cr3=", reinterpret_cast<addr_t>(cr3), ", cr4=", reinterpret_cast<addr_t>(cr4),
                 ", cr8=", reinterpret_cast<addr_t>(cr8), '\n');

    trace::print("cpu id=", arch::cpu::current().get_id(), " apic id = ", arch::cpu::current().get_apic_id(), " \n");

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("end of registers.", '\n');
    trace::print_reset();
    return reinterpret_cast<addr_t>(rbp);
}
