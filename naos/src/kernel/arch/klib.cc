#include "kernel/arch/klib.hpp"
#include "common.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/cpu.hpp"
#include "kernel/ksybs.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/zone.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/memory.hpp"

int get_stackframes_by(u64 rbp, u64 rsp, u64 rip, int skip, stack_frame_t *frames, int count)
{
    int i = 0;
    int used = 0;
    // u64 end = (u64)_file_end + (u64)base_virtual_addr;
    // u64 start = (u64)_file_start + (u64)base_virtual_addr;
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
        if (!arch::paging::has_map(arch::paging::current(), p))
        {
            break;
        }
        rip = *p;
        // if (rip > end || rip < start)
        // {
        //     break;
        // }
        rbp = reinterpret_cast<u64>(*reinterpret_cast<u64 *>(rbp));
    }
    // util::memzero(frames + used * sizeof(stack_frame_t), (count - used) * sizeof(stack_frame_t));
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
    const char *func = ksybs::get_symbol_name(reinterpret_cast<addr_t>(rip));
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
        trace::print("\n    [", reinterpret_cast<addr_t>(i), "]=", reinterpret_cast<addr_t>(*val_of_i));
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
        trace::print("\nrax=", reinterpret_cast<addr_t>(regs->rax), ", rbx=", reinterpret_cast<addr_t>(regs->rbx),
                     ", rcx=", reinterpret_cast<addr_t>(regs->rcx), ", rdx=", reinterpret_cast<addr_t>(regs->rdx),
                     ", r8=", reinterpret_cast<addr_t>(regs->r8), ", r9=", reinterpret_cast<addr_t>(regs->r9),
                     ",\n r10=", reinterpret_cast<addr_t>(regs->r10), ", r11=", reinterpret_cast<addr_t>(regs->r11),
                     ", r12=", reinterpret_cast<addr_t>(regs->r12), ", r13=", reinterpret_cast<addr_t>(regs->r13),
                     ", r14=", reinterpret_cast<addr_t>(regs->r14), ", r15=", reinterpret_cast<addr_t>(regs->r15),
                     ",\n rdi=", reinterpret_cast<addr_t>(regs->rdi), ", rsi=", reinterpret_cast<addr_t>(regs->rsi),
                     ", cs=", reinterpret_cast<addr_t>(regs->cs), ", ds=", reinterpret_cast<addr_t>(regs->ds),
                     ", es=", reinterpret_cast<addr_t>(regs->es), ", fs=", reinterpret_cast<addr_t>(fs),
                     ",\n gs=", reinterpret_cast<addr_t>(gs), ", ss=", reinterpret_cast<addr_t>(regs->ss),
                     ", rip=", reinterpret_cast<addr_t>(regs->rip), ", rsp=", reinterpret_cast<addr_t>(regs->rsp),
                     ", rbp=", reinterpret_cast<addr_t>(regs->rbp), ", rflags=", reinterpret_cast<addr_t>(regs->rflags),
                     ",\n vector=", reinterpret_cast<addr_t>(regs->vector),
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
                       ",\n rip=", reinterpret_cast<addr_t>(reg_rip), ", rsp=", reinterpret_cast<addr_t>(reg_rsp),
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
    u64 free_pages = memory::global_zones->pages_count();
    trace::print<trace::PrintAttribute<trace::CBK::Black, trace::CFG::White>>("system info: \n");
    trace::print("buddy free pages ", free_pages, "\n");
    trace::print_reset();
    return reinterpret_cast<addr_t>(rbp);
}
