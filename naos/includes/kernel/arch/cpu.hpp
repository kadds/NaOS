#pragma once
#include "common.hpp"
#include <atomic>

namespace task
{
struct thread_t;
};
namespace arch::cpu
{
constexpr u32 max_cpu_support = 32;
using cpuid_t = u32;

struct cpu_t
{
  private:
    cpuid_t id;
    void *volatile kernel_rsp;

    /// The interrupt context RSP value
    void *interrupt_rsp;
    void *exception_rsp;
    void *exception_ext_rsp;
    void *soft_irq_rsp;
    volatile std::atomic_bool is_in_soft_irq = false;
    volatile u64 soft_irq_pending = 0;

    ::task::thread_t *volatile task = nullptr;

  public:
    cpu_t() = default;
    cpu_t(const cpu_t &) = delete;
    cpu_t &operator=(const cpu_t &) = delete;

    friend cpuid_t init();
    friend void init_data(cpuid_t id);

    bool in_soft_irq() { return is_in_soft_irq; }

    void exit_soft_irq() { is_in_soft_irq = false; }

    void enter_soft_irq() { is_in_soft_irq = true; }

    void set_irq_pending(int index) { soft_irq_pending |= (1 << index); }

    void clean_irq_pending(int index) { soft_irq_pending &= ~(1 << index); }

    bool is_irq_pending(int index) { return soft_irq_pending & (1 << index); }

    void set_context(::task::thread_t *task);

    cpuid_t get_id() { return id; }

    void *get_kernel_rsp() { return kernel_rsp; }

    void *get_interrupt_rsp() { return interrupt_rsp; }

    void *get_exception_rsp() { return exception_rsp; }

    void *get_exception_ext_rsp() { return exception_ext_rsp; }

    void *get_soft_irq_rsp() { return soft_irq_rsp; }

    ::task::thread_t *get_task() { return task; }

    bool is_in_exception_context();
    bool is_in_hard_irq_context();
    bool is_in_soft_irq_context();
    bool is_in_irq_context();
    bool is_in_kernel_context();

    bool is_in_exception_context(void *rsp);
    bool is_in_soft_irq_context(void *rsp);
    bool is_in_hard_irq_context(void *rsp);
    bool is_in_irq_context(void *rsp);
    bool is_in_kernel_context(void *rsp);
    bool is_in_user_context(void *rsp);

    bool has_task() { return task != nullptr; }

    bool is_bsp();

}; // storeage in gs
extern cpu_t pre_cpu_data[];

cpuid_t init();
void init_data(cpuid_t id);

cpu_t &get(cpuid_t cpuid);
cpu_t &current();

cpuid_t id();

} // namespace arch::cpu
