#pragma once
#include "common.hpp"
#include <atomic>

namespace task
{
struct thread_t;
}

namespace arch::cpu
{
constexpr u32 max_cpu_support = 32;
using cpuid_t = u32;

struct cpu_t
{
  private:
    cpuid_t id;
    /// kernel stack pointer
    byte *volatile kernel_rsp;
    void *user_data = nullptr;

    /// The interrupt context RSP value
    byte *interrupt_rsp;
    byte *exception_rsp;
    byte *exception_nmi_rsp;
    std::atomic_bool is_in_soft_irq = false;
    volatile u64 soft_irq_pending = 0;
    /// arch apic id
    u64 apic_id;

  public:
    cpu_t() = default;
    cpu_t(const cpu_t &) = delete;
    cpu_t &operator=(const cpu_t &) = delete;

    friend cpuid_t init();
    friend void init_data(cpuid_t id);

    bool in_soft_irq() { return is_in_soft_irq; }

    void exit_soft_irq() { is_in_soft_irq = false; }

    void enter_soft_irq() { is_in_soft_irq = true; }

    void set_irq_pending(int index) { soft_irq_pending = soft_irq_pending | (1 << index); }

    void clean_irq_pending(int index) { soft_irq_pending = soft_irq_pending & ~(1 << index); }

    bool is_irq_pending(int index) { return soft_irq_pending & (1 << index); }

    void set_context(void *stack);

    cpuid_t get_id() { return id; }

    u64 get_apic_id() { return apic_id; }

    byte *get_kernel_rsp() { return kernel_rsp; }

    byte *get_interrupt_rsp() { return interrupt_rsp; }

    byte *get_exception_rsp() { return exception_rsp; }

    byte *get_exception_nmi_rsp() { return exception_nmi_rsp; }

    bool is_in_exception_context();
    bool is_in_interrupt_context();
    bool is_in_kernel_context();

    bool is_in_exception_context(void *rsp);
    bool is_in_interrupt_context(void *rsp);
    bool is_in_kernel_context(void *rsp);

    bool is_bsp();

    void *get_user_data() { return user_data; }
    void set_user_data(void *ud) { user_data = ud; }
    void set_apic_id(u64 id) { apic_id = id; }

}; // storeage in gs
extern cpu_t pre_cpu_data[];

cpuid_t init();
void init_data(cpuid_t id);

cpu_t &get(cpuid_t cpuid);
cpu_t &current();
cpu_t &fast_current();
void *current_user_data();

u64 count();

cpuid_t id();

bool has_init();

void allocate_ap_stack(int num);
void allocate_bsp_stack();

phy_addr_t get_kernel_stack_bottom_phy(cpuid_t id);
phy_addr_t get_exception_stack_bottom_phy(cpuid_t id);
phy_addr_t get_interrupt_stack_bottom_phy(cpuid_t id);
phy_addr_t get_exception_nmi_stack_bottom_phy(cpuid_t id);

byte *get_kernel_stack_bottom(cpuid_t id);
byte *get_exception_stack_bottom(cpuid_t id);
byte *get_interrupt_stack_bottom(cpuid_t id);
byte *get_exception_nmi_stack_bottom(cpuid_t id);

} // namespace arch::cpu
