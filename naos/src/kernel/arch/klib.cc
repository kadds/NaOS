#include "kernel/arch/klib.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/common.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/zone.hpp"
#include "kernel/task.hpp"

KLOG_MODULE(arch);
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
    if (memory::kernel_vm_info && memory::kernel_vm_info->paging().get_map(reinterpret_cast<void *>(rbp)).has_value())
    {
        KLOG_RAW("stack data(rbp->rsp):");
        u64 end_rbp = rbp - sizeof(u64) * 10;
        if (end_rbp < rsp)
        {
            end_rbp = rsp;
        }
        KLOG_RAW(" [{}-{}]", log::hex(rbp), log::hex(end_rbp));

        (void)0;

        u64 tmp_rbp = rbp;
        int n = 0;
        while (tmp_rbp > end_rbp)
        {
            if ((n++ % 4) == 0)
            {
                KLOG_RAW("\n");
            }
            /// print stack value
            u64 *val0 = reinterpret_cast<u64 *>(tmp_rbp);
            KLOG_RAW("{}  ", log::hex(*val0));
            tmp_rbp -= sizeof(u64);
        }

        KLOG_RAW("\nend of stack data.\n");
    }

    int n = get_stackframes_by(rbp, rsp, rip, 2, frames, frame_count);
    if (n > 0)
    {
        KLOG_RAW("stack trace:\n");
        (void)0;
        for (int i = 0; i < n; i++)
        {
            auto &frame = frames[i];
            KLOG_RAW("{} rbp:{}\n", log::hex(frame.rip), log::hex(frame.rbp));
        }

        KLOG_RAW("end of stack trace. \n");
    }

    if (regs != nullptr)
    {
        u64 fs, gs;
        __asm__ __volatile__("movq %%fs, %0 \n\t movq %%gs, %1\n\t" : "=r"(fs), "=r"(gs) : :);
        KLOG_RAW("registers(with intr regs):");
        (void)0;
        KLOG_RAW("\nrax={}, rbx={}, rcx={}, rdx={}, r8={}, r9={}, r10={}, r11={}, r12={}, r13={}, r14={}, r15={}, "
                 "rdi={}, rsi={}, cs={}, ds={}, es={}, fs={}, gs={}, ss={}, rip={}, rsp={}, rbp={}, rflags={}, "
                 "vector={}, error_code={}\n",
                 log::hex(regs->rax), log::hex(regs->rbx), log::hex(regs->rcx), log::hex(regs->rdx), log::hex(regs->r8),
                 log::hex(regs->r9), log::hex(regs->r10), log::hex(regs->r11), log::hex(regs->r12), log::hex(regs->r13),
                 log::hex(regs->r14), log::hex(regs->r15), log::hex(regs->rdi), log::hex(regs->rsi), log::hex(regs->cs),
                 log::hex(regs->ds), log::hex(regs->es), log::hex(fs), log::hex(gs), log::hex(regs->ss),
                 log::hex(regs->rip), log::hex(regs->rsp), log::hex(regs->rbp), log::hex(regs->rflags),
                 log::hex(regs->vector), log::hex(regs->error_code));
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

        KLOG_RAW("registers(without intr regs):");
        (void)0;
        KLOG_RAW("\ncs={}, ds={}, es={}, fs={}, gs={}, ss={}, rip={}, rsp={}, rbp={}, rflags={}\n", log::hex(cs),
                 log::hex(ds), log::hex(es), log::hex(fs), log::hex(gs), log::hex(ss), log::hex(reg_rip),
                 log::hex(reg_rsp), log::hex(reg_rbp), log::hex(reg_rflags));
    }
    u64 gs_base, k_gs_base, fs_base;

    k_gs_base = _rdmsr(0xC0000102);
    gs_base = _rdmsr(0xC0000101);
    fs_base = _rdmsr(0xC0000100);
    KLOG_RAW("msr(now): kernel_gs_base={}, gs_base={}, fs_base={}\n", log::hex(k_gs_base), log::hex(gs_base),
             log::hex(fs_base));
    u64 cr0, cr2, cr3, cr4, cr8;
    __asm__ __volatile__(
        "movq %%cr0, %0 \n\t movq %%cr2, %1\n\t  movq %%cr3, %2\n\t  movq %%cr4,  %3\n\t  movq %%cr8, %4\n\t "
        : "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4), "=r"(cr8)
        :
        :);

    KLOG_RAW("control registers(now): cr0={}, cr2={}, cr3={}, cr4={}, cr8={}\n", log::hex(cr0), log::hex(cr2),
             log::hex(cr3), log::hex(cr4), log::hex(cr8));

    if (arch::cpu::has_init())
    {
        auto &cpu = arch::cpu::current();

        KLOG_RAW("cpu id={} apic id = {} \n", cpu.get_id(), cpu.get_apic_id());
    }

    KLOG_RAW("end of registers.\n");
    KLOG_RAW("system info: \n");

    if (memory::global_zones != nullptr)
    {
        KLOG_RAW("buddy free pages {}/{}. free {}Mib \n", memory::global_zones->free_pages(),
                 memory::global_zones->total_pages(),
                 memory::global_zones->free_pages() * memory::page_size / 1024 / 1024);
    }
    (void)0;
    return log::hex(rbp);
}
