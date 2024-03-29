#include "kernel/arch/task.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/regs.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

namespace arch::task
{

void init(::task::thread_t *thd, register_info_t *register_info)
{
    u64 stack_low;
    __asm__ __volatile__("andq %%rsp,%0	\n\t" : "=r"(stack_low) : "0"(~(memory::kernel_stack_size - 1)));
    register_info->cr2 = 0;
    register_info->error_code = 0;
    register_info->rsp = (void *)(stack_low + memory::kernel_stack_size);
    register_info->trap_vector = 0;
    register_info->task = thd;

    thd->kernel_stack_top = (void *)register_info->rsp;

    _wrmsr(0xC0000080, _rdmsr(0xC0000080) | 1);
    constexpr u64 selector = ((u64)(gdt::gen_selector(arch::gdt::selector_type::kernel_code, 0)) << 32) |
                             ((u64)(gdt::gen_selector(arch::gdt::selector_type::user_code, 3) - 16) << 48);
    _wrmsr(0xC0000081, selector);
    _wrmsr(0xC0000082, (u64)&_sys_call);
    _wrmsr(0xC0000084, 0x200); // rFLAGS clean bits
}

ExportC void switch_task(register_info_t *prev, register_info_t *next) {}

ExportC void do_exit(u64 exit_code)
{
    trace::debug("Thread exit unexpected!");
    while (1)
        ;
}

u64 create_thread(::task::thread_t *thd, void *function, u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    regs_t regs;
    memset(&regs, 0, sizeof(regs));

    regs.rbx = (u64)function;
    regs.rdi = arg0;
    regs.rsi = arg1;
    regs.rdx = arg2;
    regs.rcx = arg3;

    regs.ds = gdt::gen_selector(gdt::selector_type::kernel_data, 0);
    regs.es = regs.ds;
    regs.cs = gdt::gen_selector(gdt::selector_type::kernel_code, 0);
    regs.ss = regs.ds;
    regs.rflags = (1 << 9); // enable IF
    regs.rip = (u64)_kernel_thread;
    auto &register_info = *thd->register_info;
    register_info.rip = (void *)regs.rip;
    register_info.rsp = (void *)((u64)thd->kernel_stack_top - sizeof(regs));
    register_info.task = thd;

    memcpy((void *)((u64)thd->kernel_stack_top - sizeof(regs)), &regs, sizeof(regs_t));

    return 0;
}

u64 enter_userland(::task::thread_t *thd, u64 stack_offset, void *entry, u64 arg0, u64 arg1)
{
    regs_t regs;
    memset(&regs, 0, sizeof(regs));
    thd->register_info->rip = (void *)&_sys_ret;
    thd->register_info->task = thd;
    regs.rcx = (u64)entry;
    regs.r10 = (u64)(thd->user_stack_top) - stack_offset;
    regs.rsp = regs.r10;
    regs.r11 = (1 << 9); // rFLAGS: enable IF
    regs.ds = gdt::gen_selector(gdt::selector_type::user_data, 3);
    regs.es = regs.ds;
    regs.cs = gdt::gen_selector(gdt::selector_type::user_code, 3);
    regs.rdi = arg0;
    regs.rsi = arg1;

    uctx::UninterruptibleContext icu;
    // write fs
    update_fs(thd);

    _call_sys_ret(&regs);
    return 1;
}

u64 enter_userland(::task::thread_t *thd, void *entry, u64 arg0, u64 arg1)
{
    regs_t regs;
    memset(&regs, 0, sizeof(regs));
    thd->register_info->rip = (void *)&_sys_ret;
    thd->register_info->task = thd;
    regs.rcx = (u64)entry;
    regs.r10 = (u64)thd->user_stack_top;
    regs.rsp = regs.r10;
    regs.r11 = (1 << 9); // rFLAGS: enable IF
    regs.ds = gdt::gen_selector(gdt::selector_type::user_data, 3);
    regs.es = regs.ds;
    regs.cs = gdt::gen_selector(gdt::selector_type::user_code, 3);
    regs.rdi = arg0;
    regs.rsi = arg1;

    uctx::UninterruptibleContext icu;
    // write fs
    update_fs(thd);

    _call_sys_ret(&regs);
    return 1;
}

void enter_userland(::task::thread_t *thd, regs_t &regs)
{
    uctx::UninterruptibleContext icu;
    // write fs
    update_fs(thd);

    _call_sys_ret(&regs);
}

void get_syscall_regs(regs_t &regs)
{
    uctx::UninterruptibleContext icu;
    auto thread = ::task::current();
    memcpy(&regs, reinterpret_cast<byte *>(thread->kernel_stack_top) - sizeof(regs), sizeof(regs));
}

void update_fs(::task::thread_t *thd)
{
    u64 tcb = reinterpret_cast<u64>(thd->tcb);
    _wrmsr(0xC0000100, tcb);
    kassert(_rdmsr(0xC0000100) == tcb, "Unreadable fs base ", trace::hex(tcb), " ", trace::hex(_rdmsr(0xC0000100)));
}

bool make_signal_context(void *stack, void *func, userland_code_context *context)
{
    u64 rsp;
    __asm__ __volatile__("movq %%rsp, %0\n\t" : "=g"(rsp) : :);
    regs_t *regs;
    auto &cpu = arch::cpu::current();
    if (cpu.is_in_exception_context((void *)rsp) || cpu.is_in_interrupt_context((void *)rsp))
    {
        if (cpu.is_in_exception_context((void *)rsp))
        {
            rsp = (u64)cpu.get_exception_rsp();
        }
        else
        {
            rsp = (u64)cpu.get_interrupt_rsp();
        }
        regs = (regs_t *)(rsp - sizeof(regs_t));
        rsp = regs->rsp;
        rsp = rsp & ~(7); // align

        if (rsp <= (u64)::task::current()->user_stack_bottom + 256)
        {
            // stack too small
            return false;
        }

        context->regs = *regs; // convent to syscall stack frame
        context->regs.rcx = regs->rip;

        regs->rip = (u64)func;
        context->data_stack_rsp = (u64)rsp;
        context->rsp = &regs->rsp;
        context->param[0] = &regs->rdi;
        context->param[1] = &regs->rsi;
        context->param[2] = &regs->rdx;
        context->param[3] = &regs->rcx;
    }
    else if (cpu.is_in_kernel_context((void *)rsp))
    {
        rsp = (u64)cpu.get_kernel_rsp();
        regs = (regs_t *)(rsp - sizeof(regs_t));
        rsp = regs->rsp - 128; // red zone
        rsp = rsp & ~(7);      // align
        if (rsp <= (u64)::task::current()->user_stack_bottom + 256)
        {
            // stack too small
            return false;
        }

        context->regs = *regs;
        regs->rcx = (u64)func;
        context->data_stack_rsp = (u64)rsp;
        context->rsp = &regs->rsp;
        context->param[0] = &regs->rdi;
        context->param[1] = &regs->rsi;
        context->param[2] = &regs->rdx;
        context->param[3] = &regs->r12;
    }
    else
    {
        trace::panic("Unknown rsp value");
    }
    return true;
}

u64 copy_signal_param(userland_code_context *context, const void *ptr, u64 size)
{
    size = (size + 7) & ~(7); // align
    context->data_stack_rsp -= size;
    memcpy((void *)context->data_stack_rsp, ptr, size);
    return context->data_stack_rsp;
}

void set_signal_param(userland_code_context *context, int index, u64 val) { *context->param[index] = val; }

void set_signal_context(userland_code_context *context) { *context->rsp = context->data_stack_rsp; }

void return_from_signal_context(userland_code_context *context)
{
    u64 rsp;
    __asm__ __volatile__("movq %%rsp, %0\n\t" : "=g"(rsp) : :);
    regs_t *regs;
    auto &cpu = arch::cpu::current();
    if (cpu.is_in_kernel_context((void *)rsp))
    {
        rsp = (u64)cpu.get_kernel_rsp();
        regs = (regs_t *)(rsp - sizeof(regs_t));
        *regs = context->regs;
    }
    else
    {
        trace::panic("Unknown rsp value");
    }
}

struct xsave_header_t
{
    u64 state_bv;
    u64 comp_bv;
    char pad[48];
};

static_assert(sizeof(xsave_header_t) == 64, "");

register_info_t *new_register(bool init_sse)
{
    auto info = memory::KernelCommonAllocatorV->New<register_info_t>();
    memset(info, 0, sizeof(register_info_t));
    if (init_sse)
    {
        info->sse_context = memory::KernelBuddyAllocatorV->allocate(4096, 64);
        memset(info->sse_context, 0, 4096);
        auto ptr = reinterpret_cast<char *>(info->sse_context);
        auto header = reinterpret_cast<xsave_header_t *>(ptr + 512);
        // state_bv must same as XSETBV (sdm 13.8.1)
        // else GP(0) is raised
        // only sse registers is supported
        header->state_bv = 0x3;
        header->comp_bv = 0;
    }
    return info;
}

void delete_register(register_info_t *info)
{
    if (info->sse_context)
    {
        memory::KernelBuddyAllocatorV->deallocate(info->sse_context);
    }
    memory::KernelCommonAllocatorV->Delete(info);
}
} // namespace arch::task

ExportC void save_sse_context()
{

    auto current = task::current();
    auto reg = current->register_info;
    if (reg->sse_context && !reg->sse_saved)
    {
        void *save_to = reg->sse_context;
        __asm__ __volatile__("movl $0xFFFFFFFF, %%eax\n\t"
                             "movl $0xFFFFFFFF, %%edx\n\t"
                             "XSAVE (%0)\n\t"
                             :
                             : "c"(save_to)
                             : "rax", "rdx");
        reg->sse_saved = true;
    }
}

ExportC void load_sse_context()
{
    auto current = task::current();
    auto reg = current->register_info;
    if (reg->sse_context && reg->sse_saved)
    {
        void *restore_from = reg->sse_context;
        __asm__ __volatile__("movl $0xFFFFFFFF, %%eax\n\t"
                             "movl $0xFFFFFFFF, %%edx\n\t"
                             "XRSTOR (%0)\n\t"
                             :
                             : "c"(restore_from)
                             : "rax", "rdx");
        reg->sse_saved = false;
    }
}