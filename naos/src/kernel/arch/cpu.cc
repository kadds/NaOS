#include "kernel/arch/cpu.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/task.hpp"
#include "kernel/arch/tss.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/mm.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace arch::cpu
{
cpu_t per_cpu_data[max_cpu_support];
std::atomic_int last_cpuid = 0;

void *get_rsp()
{
    void *stack;
    __asm__ __volatile__("movq %%rsp,%0	\n\t" : "=r"(stack) : :);
    return stack;
}

// switch task
void cpu_t::set_context(void *stack)
{
    kernel_rsp = static_cast<byte *>(stack);
    tss::set_rsp(cpu::current().get_id(), 0, stack);
}

bool cpu_t::is_in_exception_context() { return is_in_exception_context(get_rsp()); }

bool cpu_t::is_in_kernel_context() { return is_in_kernel_context(get_rsp()); }

bool cpu_t::is_in_interrupt_context() { return is_in_interrupt_context(get_rsp()); }

bool cpu_t::is_in_exception_context(void *rsp)
{
    byte *t_rsp = get_exception_rsp();
    byte *b_rsp = t_rsp - memory::exception_stack_size;
    byte *s_rsp = static_cast<byte *>(rsp);

    return s_rsp <= t_rsp && s_rsp >= b_rsp;
}

bool cpu_t::is_in_interrupt_context(void *rsp)
{
    byte *t_rsp = get_interrupt_rsp();
    byte *b_rsp = t_rsp - memory::interrupt_stack_size;
    byte *s_rsp = static_cast<byte *>(rsp);

    return s_rsp <= t_rsp && s_rsp >= b_rsp;
}

bool cpu_t::is_in_kernel_context(void *rsp)
{
    byte *t_rsp = get_kernel_rsp();
    byte *b_rsp = t_rsp - memory::kernel_stack_size;
    byte *s_rsp = static_cast<byte *>(rsp);

    return s_rsp <= t_rsp && s_rsp >= b_rsp;
}

cpuid_t init()
{
    u64 cpuid = last_cpuid++;
    auto &cur_data = per_cpu_data[cpuid];
    cur_data.id = cpuid;
    /// gs kernel base
    _wrmsr(0xC0000102, (u64)&per_cpu_data[cpuid]);
    kassert(_rdmsr(0xC0000102) == ((u64)&per_cpu_data[cpuid]), "Unable to write kernel gs_base");
    /// gs user base
    _wrmsr(0xC0000101, 0);
    /// fs base
    _wrmsr(0xC0000100, 0);
    __asm__("swapgs \n\t" ::: "memory");
    kassert(_rdmsr(0xC0000101) == ((u64)&per_cpu_data[cpuid]), "Unable to swap kernel gs register");

    return cpuid;
}

bool has_init() { return last_cpuid >= 1; }

void init_data(cpuid_t cpuid)
{
    _wrmsr(0xC0000101, (u64)&per_cpu_data[cpuid]);

    auto &data = cpu::current();
    data.interrupt_rsp = get_interrupt_stack_bottom(cpuid) + memory::interrupt_stack_size;
    data.exception_rsp = get_exception_stack_bottom(cpuid) + memory::exception_stack_size;
    data.exception_nmi_rsp = get_exception_nmi_stack_bottom(cpuid) + memory::exception_nmi_stack_size;

    data.kernel_rsp = get_kernel_stack_bottom(cpuid) + memory::kernel_stack_size;

    tss::set_rsp(cpuid, 0, data.kernel_rsp);
    tss::set_ist(cpuid, 1, data.interrupt_rsp);
    tss::set_ist(cpuid, 3, data.exception_rsp);
    tss::set_ist(cpuid, 4, data.exception_nmi_rsp);

    _enable_sse();
}

bool cpu_t::is_bsp() { return id == 0; }

cpu_t &get(cpuid_t cpuid) { return per_cpu_data[cpuid]; }

cpu_t &current()
{
    u64 cpuid;
#ifdef _DEBUG
    kassert(_rdmsr(0xC0000101) != 0, "Unreadable gs base");
    kassert(_rdmsr(0xC0000102) == 0, "Unreadable kernel gs base");
#endif
    __asm__("movq %%gs:0x0, %0\n\t" : "=r"(cpuid) : :);
    return per_cpu_data[cpuid];
}
cpu_t &fast_current()
{
    u64 cpuid;
    __asm__("movq %%gs:0x0, %0\n\t" : "=r"(cpuid) : :);
    return per_cpu_data[cpuid];
}

void *current_user_data()
{
    u64 u;
    __asm__("movq %%gs:0x10, %0\n\t" : "=r"(u) : :);
    return (void *)u;
}

u64 count() { return last_cpuid; }

cpuid_t id() { return current().get_id(); }

void map(u64 &base, u64 pg, bool is_bsp = false) {
    u64 size = pg * memory::page_size;
    auto c = (arch::paging::base_paging_t *) memory::kernel_vm_info->mmu_paging.get_base_page();
    base += memory::page_size;
    phy_addr_t ks;
    if (is_bsp) {
        ks = phy_addr_t::from(0x90000 - size);
    } else {
        ks = memory::va2pa(memory::KernelBuddyAllocatorV->allocate(size, 0));
    }
    paging::map(c, reinterpret_cast<void *>(base), ks, paging::frame_size::size_4kb, pg, paging::flags::writable,
                false);
    base += size;
}

void allocate_stack(int logic_num)
{
    u64 base = memory::kernel_cpu_stack_bottom_address;
    for (int i = 0; i < logic_num; i++)
    {
        if (i != 0) {
            map(base, memory::kernel_stack_page_count);
        } else {
            map(base, memory::kernel_stack_page_count, true);
        }
        map(base, memory::exception_stack_page_count);
        map(base, memory::interrupt_stack_page_count);
        map(base, memory::exception_nmi_stack_page_count);
    }
}

phy_addr_t get_kernel_stack_bottom_phy(cpuid_t id) {
    void *vir = get_kernel_stack_bottom(id);
    phy_addr_t phy = nullptr;
    bool ret = paging::get_map_address(paging::current(), vir, &phy);
    kassert(ret, "");
    return phy;
}

phy_addr_t get_exception_stack_bottom_phy(cpuid_t id) {
    void *vir = get_exception_stack_bottom(id);
    phy_addr_t phy = nullptr;
    bool ret = paging::get_map_address(paging::current(), vir, &phy);
    kassert(ret, "");
    return phy;
}

phy_addr_t get_interrupt_stack_bottom_phy(cpuid_t id) {

    void *vir = get_interrupt_stack_bottom(id);
    phy_addr_t phy = nullptr;
    bool ret = paging::get_map_address(paging::current(), vir, &phy);
    kassert(ret, "");
    return phy;
}

phy_addr_t get_exception_nmi_stack_bottom_phy(cpuid_t id) {
    void *vir = get_exception_nmi_stack_bottom(id);
    phy_addr_t phy = nullptr;
    bool ret = paging::get_map_address(paging::current(), vir, &phy);
    kassert(ret, "");
    return phy;
}

byte *get_kernel_stack_bottom(cpuid_t id)
{
    u64 base = memory::kernel_cpu_stack_bottom_address;
    u64 each_of_cpu_size = memory::kernel_stack_size + memory::exception_stack_size + memory::interrupt_stack_size +
                           memory::exception_nmi_stack_size;

    each_of_cpu_size += memory::page_size * 4; // guard pages

    return reinterpret_cast<byte *>(base + each_of_cpu_size * id + memory::page_size);
}

byte *get_exception_stack_bottom(cpuid_t id)
{
    u64 base = memory::kernel_cpu_stack_bottom_address;
    u64 each_of_cpu_size = memory::kernel_stack_size + memory::exception_stack_size + memory::interrupt_stack_size +
                           memory::exception_nmi_stack_size;

    each_of_cpu_size += memory::page_size * 4; // guard pages

    return reinterpret_cast<byte *>(base + each_of_cpu_size * id + memory::page_size * 2 + memory::kernel_stack_size);
}

byte *get_interrupt_stack_bottom(cpuid_t id)
{
    u64 base = memory::kernel_cpu_stack_bottom_address;
    u64 each_of_cpu_size = memory::kernel_stack_size + memory::exception_stack_size + memory::interrupt_stack_size +
                           memory::exception_nmi_stack_size;

    each_of_cpu_size += memory::page_size * 4; // guard pages

    return reinterpret_cast<byte *>(base + each_of_cpu_size * id + memory::page_size * 3 + memory::kernel_stack_size +
                                    memory::exception_stack_size);
}

byte *get_exception_nmi_stack_bottom(cpuid_t id)
{
    u64 base = memory::kernel_cpu_stack_bottom_address;
    u64 each_of_cpu_size = memory::kernel_stack_size + memory::exception_stack_size + memory::interrupt_stack_size +
                           memory::exception_nmi_stack_size;

    each_of_cpu_size += memory::page_size * 4; // guard pages

    return reinterpret_cast<byte *>(base + each_of_cpu_size * id + memory::page_size * 4 + memory::kernel_stack_size +
                                    memory::exception_stack_size + memory::interrupt_stack_size);
}

} // namespace arch::cpu