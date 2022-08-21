#include "kernel/arch/klib.hpp"
#include "common.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/ksybs.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/zone.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

int get_stackframes_by(u64 rbp, u64 rsp, u64 rip, int skip, stack_frame_t *frames, int count)
{
    if (memory::kernel_vm_info == nullptr)
    {
        return 0;
    }
    int i = 0;
    int used = 0;
    while ((u64)rbp != 0)
    {
        if (++i > skip)
        {
            if (used >= count)
            {
                break;
            }
            auto &frame = frames[used];
            frame.rip = reinterpret_cast<void *>(rip);
            frame.rsp = reinterpret_cast<void *>(rsp);
            frame.rbp = reinterpret_cast<void *>(rbp);
            used++;
        }
        auto *p = reinterpret_cast<u64 *>(rbp) + 1;
        if (!is_kernel_space_pointer(p))
        {
            break;
        }
        if (!memory::kernel_vm_info->paging().has_flags(reinterpret_cast<void *>(p)))
        {
            break;
        }
        rip = *p;
        rbp = reinterpret_cast<u64>(*reinterpret_cast<u64 *>(rbp));
    }
    return used;
}

int get_stackframes(int skip, stack_frame_t *frames, int count)
{
    u64 rbp;
    u64 rsp;
    u64 rip = reinterpret_cast<u64>(&get_stackframes);
    __asm__ __volatile__("movq %%rbp, %0 \n\t" : "=g"(rbp) : :);
    __asm__ __volatile__("movq %%rsp, %0 \n\t" : "=g"(rsp) : :);
    return get_stackframes_by(rbp, rsp, rip, skip + 1, frames, count);
}

const char *stack_frame_t::get_frame_name()
{
    const char *func = ksybs::get_symbol_name(trace::hex(rip));
    return func;
}

void get_task_id(u64 &pid, u64 &tid)
{
    if (arch::cpu::has_init() && task::has_init())
    {
        auto t = task::current();

        if (t != nullptr)
        {
            tid = t->tid;
            pid = t->process->pid;
            return;
        }
    }
    tid = 0;
    pid = 0;
}

constexpr int frame_count = 24;
static stack_frame_t frames[frame_count];

void *print_stack(const regs_t *regs, int max_depth)
{
    u64 rbp;
    u64 rip = reinterpret_cast<u64>(&print_stack);
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
        rip = regs->rip;
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
        trace::print("\n    [", trace::hex(i), "]=", trace::hex(*val_of_i));
    }

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("\nend of stack data.\n");

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("stack trace:\n");
    trace::print<trace::PrintAttribute<trace::CBK::Default>>();
    int n = get_stackframes_by(rbp, rsp, rip, 2, frames, frame_count);

    for (int i = 0; i < n; i++)
    {
        auto &frame = frames[i];
        const char *fn = frame.get_frame_name();
        if (fn != nullptr)
        {
            trace::print<trace::PrintAttribute<trace::CFG::LightCyan>>(fn);
            trace::print<trace::PrintAttribute<trace::CFG::Default>>(" [", trace::hex(frame.rip), "]");
            trace::print<trace::PrintAttribute<trace::CFG::White>>(" rbp:", trace::hex(frame.rbp), "\n");
        }
        else
        {
            trace::print<trace::PrintAttribute<trace::CBK::Default, trace::CFG::Default>>(
                trace::hex(frame.rip), " rbp:", trace::hex(frame.rbp), "\n");
        }
    }

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("end of stack trace. \n");

    if (regs != nullptr)
    {
        u64 fs, gs;
        __asm__ __volatile__("movq %%fs, %0 \n\t movq %%gs, %1\n\t" : "=r"(fs), "=r"(gs) : :);
        trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("registers(with intr regs):");
        trace::print<trace::PrintAttribute<trace::TextAttribute::Reset>>();
        trace::print("\nrax=", trace::hex(regs->rax), ", rbx=", trace::hex(regs->rbx), ", rcx=", trace::hex(regs->rcx),
                     ", rdx=", trace::hex(regs->rdx), ", r8=", trace::hex(regs->r8), ", r9=", trace::hex(regs->r9),
                     ",\n r10=", trace::hex(regs->r10), ", r11=", trace::hex(regs->r11),
                     ", r12=", trace::hex(regs->r12), ", r13=", trace::hex(regs->r13), ", r14=", trace::hex(regs->r14),
                     ", r15=", trace::hex(regs->r15), ",\n rdi=", trace::hex(regs->rdi),
                     ", rsi=", trace::hex(regs->rsi), ", cs=", trace::hex(regs->cs), ", ds=", trace::hex(regs->ds),
                     ", es=", trace::hex(regs->es), ", fs=", trace::hex(fs), ",\n gs=", trace::hex(gs),
                     ", ss=", trace::hex(regs->ss), ", rip=", trace::hex(regs->rip), ", rsp=", trace::hex(regs->rsp),
                     ", rbp=", trace::hex(regs->rbp), ", rflags=", trace::hex(regs->rflags),
                     ",\n vector=", trace::hex(regs->vector), ", error_code=", trace::hex(regs->error_code), "\n");
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
        trace::print<>("\ncs=", trace::hex(cs), ", ds=", trace::hex(ds), ", es=", trace::hex(es),
                       ", fs=", trace::hex(fs), ", gs=", trace::hex(gs), ", ss=", trace::hex(ss),
                       ",\n rip=", trace::hex(reg_rip), ", rsp=", trace::hex(reg_rsp), ", rbp=", trace::hex(reg_rbp),
                       ", rflags=", trace::hex(reg_rflags), "\n");
    }
    u64 gs_base, k_gs_base, fs_base;

    k_gs_base = _rdmsr(0xC0000102);
    gs_base = _rdmsr(0xC0000101);
    fs_base = _rdmsr(0xC0000100);
    trace::print("msr(now): kernel_gs_base=", trace::hex(k_gs_base), ", gs_base=", trace::hex(gs_base),
                 ", fs_base=", trace::hex(fs_base), '\n');
    u64 cr0, cr2, cr3, cr4, cr8;
    __asm__ __volatile__(
        "movq %%cr0, %0 \n\t movq %%cr2, %1\n\t  movq %%cr3, %2\n\t  movq %%cr4,  %3\n\t  movq %%cr8, %4\n\t "
        : "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4), "=r"(cr8)
        :
        :);

    trace::print("control registers(now): cr0=", trace::hex(cr0), ", cr2=", trace::hex(cr2), ", cr3=", trace::hex(cr3),
                 ", cr4=", trace::hex(cr4), ", cr8=", trace::hex(cr8), '\n');

    trace::print("cpu id=", arch::cpu::current().get_id(), " apic id = ", arch::cpu::current().get_apic_id(), " \n");

    trace::print<trace::PrintAttribute<trace::CBK::White, trace::CFG::Red>>("end of registers.", '\n');
    trace::print<trace::PrintAttribute<trace::CBK::Black, trace::CFG::White>>("system info: \n");

    trace::print("buddy free pages ", memory::global_zones->free_pages(), "/", memory::global_zones->total_pages(),
                 "\n");
    trace::print_reset();
    return trace::hex(rbp);
}
